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

#include "cpu/o3/bac.hh"

#include <algorithm>

#include "arch/generic/pcstate.hh"
#include "base/trace.hh"
#include "config/the_isa.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "cpu/o3/ftq.hh"
#include "debug/Activity.hh"
#include "debug/BAC.hh"
#include "debug/Branch.hh"
#include "debug/Drain.hh"
#include "debug/Fetch.hh"
#include "debug/O3PipeView.hh"
#include "params/BaseO3CPU.hh"
#include "sim/full_system.hh"


using namespace gem5::branch_prediction;

namespace gem5
{

namespace o3
{

// BAC::BAC(const BACParams &params)
//     :
//     //   bpu(params.bpu)
//     bpu(nullptr),
//     decoupedStats(this)
// {
// }
BAC::BAC(CPU *_cpu, const BaseO3CPUParams &params)
    :
    preFetchPredHist(params.numThreads),

    globalFTSeqNum(0),

    cpu(_cpu),
    bpu(params.branchPred),
    wroteToTimeBuffer(false),
    fetchToBacDelay(params.fetchToBacDelay),
    decodeToFetchDelay(params.decodeToFetchDelay),
    renameToFetchDelay(params.renameToFetchDelay),
    iewToFetchDelay(params.iewToFetchDelay),
    commitToFetchDelay(params.commitToFetchDelay),
    bacToFetchDelay(params.bacToFetchDelay),
    ftqSize(params.numFTQEntries),
    fetchTargetWidth(params.fetchTagetWidth),
    numThreads(params.numThreads),




    stats(_cpu,this)
{
    fatal_if(fetchTargetWidth < params.fetchBufferSize,
            "Fetch target width should be larger than fetch buffer size!");

    for (int i = 0; i < MaxThreads; i++) {
        // pc[i].reset(params.isa[0]->newPCState());
        pc[i].reset(params.isa[0]->newPCState());
        stalls[i] = {false, false, false};
    }
}

std::string
BAC::name() const {
    return cpu->name() + ".bac";
}

// void
// BAC::init() {
//     if (!bpu)
//         fatal("Branch predictor needs to be set!\n");
// }

void
BAC::regProbePoints()
{
    ppFTQInsert = new ProbePointArg<DynInstPtr>(cpu->getProbeManager(),
                                                        "FTQInsert");
}

void
BAC::setTimeBuffer(TimeBuffer<TimeStruct> *time_buffer)
{
    // TODO:
    timeBuffer = time_buffer;

    // Create wires to get information from proper places in time buffer.
    fromFetch = timeBuffer->getWire(-fetchToBacDelay);
    fromDecode = timeBuffer->getWire(-decodeToFetchDelay);
    fromRename = timeBuffer->getWire(-renameToFetchDelay);
    fromIEW = timeBuffer->getWire(-iewToFetchDelay);
    fromCommit = timeBuffer->getWire(-commitToFetchDelay);
}

void
BAC::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
}

void
BAC::setFetchTargetQueue(FTQ * _ptr)
{
    // Set pointer to the fetch target queue
    ftq = _ptr;
}

void
BAC::startupStage()
{
    resetStage();

    // TODO: Maybe its better to do blocking from fetch??????


    // For decoupled BPU the BAC need to start together with fetch
    // so it must start up in active state.
    switchToActive();
}


void
BAC::clearStates(ThreadID tid)
{
    // bacStatus[tid] = Running;
    // ftqStatus[tid] = FTQActive;
    bacStatus[tid] = Running;
    set(pc[tid], cpu->pcState(tid));
    // basicBlockProduce[tid] = NULL;
    // basicBlockConsume[tid] = NULL;
    stalls[tid].fetch = false;
    stalls[tid].drain = false;
    stalls[tid].bpu = false;
    // ftq[tid].clear();


    // ftq->squash(tid);
    assert(ftq!=nullptr);
    ftq->resetState();
}



void
BAC::resetStage()
{

    // Setup PC and nextPC with initial state.
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        bacStatus[tid] = Running;
        set(pc[tid], cpu->pcState(tid));
        // set(static_cast<std::unique_ptr<PCStateBase>>(bpuPC[tid]), cpu->pcState(tid));

        // basicBlockProduce[tid] = NULL;
        // basicBlockConsume[tid] = NULL;
        stalls[tid].fetch = false;
        stalls[tid].drain = false;
        stalls[tid].bpu = false;

        // ftq[tid].clear();


    }

    assert(ftq!=nullptr);
    ftq->resetState();

    wroteToTimeBuffer = false;
    _status = Inactive;
}




void
BAC::drainResume()
{
    for (ThreadID i = 0; i < numThreads; ++i) {
        stalls[i].drain = false;
    }
}

void
BAC::drainSanityCheck() const
{
    assert(isDrained());

    for (ThreadID i = 0; i < numThreads; ++i) {
        assert(bacStatus[i] == Idle || stalls[i].drain);
    }

    bpu->drainSanityCheck();
}

bool
BAC::isDrained() const
{
    /* Make sure the FTQ is empty and the state of all threads is
     * idle.
     */
    for (ThreadID i = 0; i < numThreads; ++i) {
        // // Verify FTQs are drained
        // if (!ftq[i].empty())
        //     return false;

        if (!preFetchPredHist[i].empty())
            return false;

        // Return false if not idle or drain stalled
        if (bacStatus[i] != Idle) {
            return false;
        }
    }
    return true;
}



void
BAC::drainStall(ThreadID tid)
{
    assert(cpu->isDraining());
    assert(!stalls[tid].drain);
    DPRINTF(Drain, "%i: Thread drained.\n", tid);
    stalls[tid].drain = true;
}

// void
// BAC::wakeFromQuiesce()
// {
//     DPRINTF(BAC, "Waking up from quiesce\n");
//     // Hopefully this is safe
//     // @todo: Allow other threads to wake from quiesce.
//     bacStatus[0] = Running;
// }

void
BAC::switchToActive()
{
    if (_status == Inactive) {
        DPRINTF(Activity, "Activating stage.\n");

        cpu->activateStage(CPU::BACIdx);
        _status = Active;
    }
}

void
BAC::switchToInactive()
{
    if (_status == Active) {
        DPRINTF(Activity, "Deactivating stage.\n");

        cpu->deactivateStage(CPU::BACIdx);
        _status = Inactive;
    }
}

void
BAC::deactivateThread(ThreadID tid)
{
    // // Update priority list
    // auto thread_it = std::find(priorityList.begin(), priorityList.end(), tid);
    // if (thread_it != priorityList.end()) {
    //     priorityList.erase(thread_it);
    // }
}


bool
BAC::checkStall(ThreadID tid) const
{
    // if (fromFetch->)
    bool ret_val = false;

    if (stalls[tid].fetch) {
        DPRINTF(BAC,"[tid:%i] Fetch stall detected.\n",tid);
        ret_val = true;
    }

    if (stalls[tid].drain) {
        assert(cpu->isDraining());
        DPRINTF(BAC,"[tid:%i] Drain stall detected.\n",tid);
        ret_val = true;
    }

    if (stalls[tid].bpu) {
        DPRINTF(BAC,"[tid:%i] BPU stall detected.\n",tid);
        ret_val = true;
    }
    return ret_val;
}

void
BAC::updateBACStatus()
{
    //Check Running
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (bacStatus[tid] == Running ||
            bacStatus[tid] == Squashing) {

            if (_status == Inactive) {
                DPRINTF(Activity, "[tid:%i] Activating stage.\n",tid);

                // if (fetchStatus[tid] == IcacheAccessComplete) {
                //     DPRINTF(Activity, "[tid:%i] Activating fetch due to cache"
                //             "completion\n",tid);
                // }

                cpu->activateStage(CPU::BACIdx);
            }

            _status = Active;
            return;
        }
    }

    // Stage is switching from active to inactive, notify CPU of it.
    if (_status == Active) {
        DPRINTF(Activity, "Deactivating stage.\n");

        cpu->deactivateStage(CPU::BACIdx);
    }

    _status = Inactive;
}




bool
BAC::checkSignalsAndUpdate(ThreadID tid)
{
    // Check if there's a squash signal, squash if there is.
    // Check stall signals, block if necessary.

    // Check squash signals from commit.
    if (fromCommit->commitInfo[tid].squash) {

        DPRINTF(Fetch, "[tid:%i] Squashing from commit. PC = %s\n",
                        tid, *fromCommit->commitInfo[tid].pc);

        // In any case, squash the FTQ and the branch histories in the
        // FTQ first.
        squashBpuHistories(tid);
        squash(*fromCommit->commitInfo[tid].pc, tid);

        // If it was a branch mispredict on a control instruction, update the
        // branch predictor with that instruction, otherwise just kill the
        // invalid state we generated in after sequence number
        if (fromCommit->commitInfo[tid].mispredictInst &&
            fromCommit->commitInfo[tid].mispredictInst->isControl()) {

                bpu->squash(fromCommit->commitInfo[tid].doneSeqNum,
                            *fromCommit->commitInfo[tid].pc,
                             fromCommit->commitInfo[tid].branchTaken, tid);
                stats.branchMisspredict++;
        } else {
            bpu->squash(fromCommit->commitInfo[tid].doneSeqNum, tid);
            if (fromCommit->commitInfo[tid].mispredictInst) {
                DPRINTF(BAC, "[tid:%i] Squashing due to mispredict of non-"
                "control instruction: %s\n",tid,
                fromCommit->commitInfo[tid]
                        .mispredictInst->staticInst->disassemble(
                      fromCommit->commitInfo[tid]
                        .mispredictInst->pcState().instAddr()));
            } else {
                DPRINTF(BAC, "[tid:%i] Squashing due to mispredict of non-"
                "control instruction: %s\n",tid);
            }
            stats.noBranchMisspredict++;
        }

        return true;
    } else if (fromCommit->commitInfo[tid].doneSeqNum) {
        // Update the branch predictor if it wasn't a squashed instruction
        // that was broadcasted.
        bpu->update(fromCommit->commitInfo[tid].doneSeqNum, tid);
    }

    // Check squash signals from decode.
    if (fromDecode->decodeInfo[tid].squash) {
        DPRINTF(Fetch, "[tid:%i] Squashing from decode. PC = %s\n",
                        tid, *fromDecode->decodeInfo[tid].nextPC);

        // Squash.
        squashBpuHistories(tid);
        squash(*fromDecode->decodeInfo[tid].nextPC, tid);

        // Update the branch predictor.
        if (fromDecode->decodeInfo[tid].branchMispredict) {
            // bac->squash(fromDecode->decodeInfo[tid].doneSeqNum,
            //         *fromDecode->decodeInfo[tid].nextPC,
            //         fromDecode->decodeInfo[tid].branchTaken, tid,
            //         fromDecode->decodeInfo[tid].mispredictInst->staticInst,
            //         fromDecode->decodeInfo[tid].mispredictInst->pcState());
            bpu->squash(fromDecode->decodeInfo[tid].doneSeqNum,
                    *fromDecode->decodeInfo[tid].nextPC,
                    fromDecode->decodeInfo[tid].branchTaken, tid);
            stats.branchMisspredict++;
        } else {
            bpu->squash(fromDecode->decodeInfo[tid].doneSeqNum,
                              tid);
            stats.noBranchMisspredict++;
        }
        return true;
    }


    // Check squash signals from fetch.
    if (fromFetch->fetchInfo[tid].squash
        && bacStatus[tid] != Squashing) {
        DPRINTF(BAC, "Squashing from fetch with PC = %s\n",
                *fromFetch->fetchInfo[tid].nextPC);

        // Squash unless we're already squashing
        squash(*fromFetch->fetchInfo[tid].nextPC, tid);
        return true;
    }



    if (checkStall(tid)) {
        // return block(tid);
        return false;
    }

    // If at this point the FTQ is still invalid we need to wait for
    // A resteer/squash signal.
    if (!ftq->isValid(tid) && bacStatus[tid] != Idle) {
        DPRINTF(BAC, "[tid:%i] FTQ is invalid. Wait for resteer.\n", tid);

        // We should not end up here.
        // assert(false);
        bacStatus[tid] = Idle;
        return true;
    }

    // Check if the FTQ became free in that cycle.
    if ((bacStatus[tid] == FTQFull) && !ftq->isFull(tid)) {

        DPRINTF(BAC, "[tid:%i] FTQ not full anymore -> Running\n", tid);

        bacStatus[tid] = Running;
        return true;
    }

    if (bacStatus[tid] == Squashing) {
        // Switch status to running after squashing FTQ and setting the PC.
        DPRINTF(BAC, "[tid:%i] Done squashing, switching to running.\n", tid);

        bacStatus[tid] = Running;
        return true;
    }

    // If we've reached this point, we have not gotten any signals that
    // cause decode to change its status.  Decode remains the same as before.
    return false;
}




void
BAC::squashBpuHistories(ThreadID tid)
{
    DPRINTF(BAC, "[tid:%i] %s: FTQ sz: %i\n", tid, __func__,
                            ftq->size(tid));

    unsigned n_fts = ftq->size(tid);
    if (n_fts == 0) return;

    /** Iterate over the FTQ in reverse order to
     * revert all predictions made.
     *  */
    FTQ::FetchTargetIt it = ftq->tail(tid);
    for (; n_fts > 0; it--, n_fts--) {

        if ((*it)->hasBranch() && (*it)->bpu_history) {
            auto hist = static_cast<BPredUnit::PredictorHistory*>
                                                ((*it)->bpu_history);
            bpu->squashHistory(tid, hist);
            (*it)->bpu_history = nullptr;
        }
    }
}



void
BAC::squash(const PCStateBase &new_pc, ThreadID tid)
{
    DPRINTF(BAC, "[tid:%i] Squashing FTQ.\n", tid);

    // Set status to squashing.
    bacStatus[tid] = Squashing;

    // Set the new PC
    set(pc[tid], new_pc);

    // Go through all fetch targets and delete them the branch
    // predictor histories.
    // squashBpuHistories(tid);

    // Then squash all fetch targets
    ftq->squash(tid);
}




void
BAC::tick()
{
    bool activity = false;
    bool status_change = false;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();


    //Check stall and squash signals.
    while (threads != end) {
        ThreadID tid = *threads++;
#ifdef FDIP
        DPRINTF(BAC,"Processing [tid:%i]\n",tid);
        status_change |= checkSignalsAndUpdate(tid);


        /** Generate fetch targets if BAC is in running state */
        if (bacStatus[tid] == Running) {
            generateFetchTargets(tid, status_change);
            activity = true;
        } else {
            DPRINTF(BAC, "FTQ Inactive\n");
        }

        profileCycle(tid);

#else
    if (bacStatus[tid] != Idle) {
        bacStatus[tid] = Idle;
        status_change = true;
    }
#endif
    }



    if (status_change) {
        updateBACStatus();
    }

    if (activity) {
        DPRINTF(Activity, "Activity this cycle.\n");

        cpu->activityThisCycle();
    }
}



FetchTargetPtr
BAC::newFetchTarget(ThreadID tid, const PCStateBase &start_pc)
{
    auto ft = std::make_shared<FetchTarget>(tid, start_pc,
                                         getAndIncrementFTSeq());

    DPRINTF(BAC, "Create new FT:[fn:%llu]\n", ft->ftNum());
    return ft;
}



void
BAC::generateFetchTargets(ThreadID tid, bool &status_change)
{

    // DPRINTF(BAC, "%s:\n",__func__);


    /**
     * This function implements the head of the decoupled frontend.
     * Instead of waiting for the pre-decoding the current instruction the
     * BTB is leveraged for finding branches in the instruction stream.
     * 1. Starting from the beginning of a basic block we check for all
     *    consecutive address if a branch if found in the BTB.
     * 2. As soon as the BTB hits, we know we have reached the end of the BB
     *    and it will be inserted as fetch target into the FTQ.
     * 3. Furthermore, for the determining branch the direction and the target
     *    is
     * - In case of a BTB hit the branch predictor will be queried to get
     *   the direction.
     * - For a BTB miss a non control instruction is assumed and continues
     *   to the next instruction.
     *
     */

    bool branchFound = false;
    bool predict_taken = false;
    unsigned numAddrSearched = 0;
    // unsigned addrDistance = 4;
    // unsigned inst_width = 1;
    // InstSeqNum seqNum;
    Addr curAddr;

    // The current PC.
    PCStateBase &this_pc = *pc[tid];
    // std::unique_ptr<PCStateBase> old_pc(this_pc.clone());


    DPRINTF(Fetch, "PC = %s, sz:%i ptr:%x\n",
                    this_pc, this_pc.as<X86ISA::PCState>().size(),
                    &this_pc);

    // Create two temporal branches that will be used fot the branch
    // predictor to search each individual address.
    BpuPCStatePtr search_pc = std::make_unique<BpuPCState>();
    // BpuPCStatePtr search_pc_tmp = std::make_unique<BpuPCState>();
    search_pc->update(this_pc);
    // search_pc_tmp->update(this_pc);

    // // BasicBlockPtr curFT = basicBlockProduce[tid];
    // // BasicBlockPtr curFT = NULL;

    // // If there is no open BB create a new basic block starting from
    // // the current PC and then search until the end is reached.
    // // if (curFT == NULL) {
    //     curFT = new BasicBlock(tid, this_pc);
    //     basicBlockProduce[tid] = curFT;
    //     DPRINTF(BAC, "Create new BB:%#x. FTQ size:%i\n",
    //         curFT, ftq[tid].size());
    // }


        // BasicBlockPtr curFT = basicBlockProduce[tid];
    // BasicBlockPtr curFT = NULL;

    // If there is no open BB create a new basic block starting from
    // the current PC and then search until the end is reached.
    // if (curFT == NULL) {

    // In each cycles we will create a new fetch target.
    FetchTargetPtr curFT = newFetchTarget(tid, this_pc);
    // basicBlockProduce[tid] = curFT;





    // DynInstPtr inst = buildInstPlaceholder();

    // Loop through the predicted instruction stream and try to find the
    // the terminating branch.
    // seqNum = cpu->getAndIncrementFTSeq();
    // seqNum = getAndIncrementFTSeq();
    while (numAddrSearched < fetchTargetWidth) {

        // Start by creating a new sequence number
        // seqNum = cpu->getAndIncrementInstSeq();
        curAddr = search_pc->instAddr();

        // search_pc_tmp->update(*search_pc);

        // Check if the current search address can be found in the BTB
        // indicating the end of the branch.
        branchFound = bpu->BTBValid(curAddr, tid);

        // If its a branch stop searching
        if (branchFound) {
            break;
        }

        // Not a branch instruction
        // - Add the sequence number to the basic block
        // - Add always two sequence numbers
        // - Advance the PC.
        // - Continue searching.
        //
        // curFT->addSeqNum(seqNum);
        // curFT->addSeqNum(cpu->getAndIncrementInstSeq());
        search_pc->advance();

        // this_pc.advance();
        numAddrSearched++;

        // DPRINTF(BAC, "[tid:%i] [sn:%llu] No branch instruction PC %#x "
        //                 "Next PC: %#x\n",
        //                 tid, seqNum, curAddr, search_pc->instAddr());
    }

    // Update the current PC to point to the last instruction
    // in the fetch target
    set(this_pc, *search_pc);
    // Depending on the branch prediction we might need
    // std::unique_ptr<PCStateBase> next_pc(this_pc.clone());




    // set(old_pc, *tmp_pc2);

    // Search stopped either by a branch found in instruction stream
    // or the maximum search width per cycle was reached.

    if (branchFound) {

        // Branch found in instruction stream. Perform the following steps
        // - Lookup the static instruction from the BTB.
        // - Build a dynamic instruction based on the static instruction
        //   returned from the BTB.
        // - Predict the branch direction as well as the branch target.
        // - Finalize the BB and insert it into the FTQ.
        StaticInstPtr staticInst = bpu->BTBLookupInst(this_pc,tid);
        assert(staticInst);

        // // Create dynamic instruction for branch here:
        // DynInstPtr inst = buildInst(tid, staticInst, nullptr, this_pc,
        //                             this_pc, seqNum, false, false);
        // std::unique_ptr<PCStateBase> next_pc(this_pc.clone());

        // Now predict the direction. Note the BP will advance
        // the PC to the next instruction.
        predict_taken = predict(staticInst, curFT, *search_pc, tid);

        // DPRINTF(BAC, "[tid:%i] [sn:%llu] Branch found at PC %#x "
        //         "predicted: %s, target: %s\n",
        //         tid, seqNum, curAddr,
        //         predict_taken ? "taken" : "not taken", this_pc);

        if (predict_taken) {
            DPRINTF(BAC, "[tid:%i] [fn:%llu] Branch at PC %s "
                    "predicted to be taken to %s: sz:%i\n",
                    tid, curFT->ftNum(), this_pc, *search_pc,
                    search_pc->as<X86ISA::PCState>().size());
        } else {
            DPRINTF(BAC, "[tid:%i] [fn:%llu] Branch at PC %s "
                    "predicted to be not taken\n",
                    tid, curFT->ftNum(), this_pc);
        }



        ++stats.branches;

        if (predict_taken) {
            ++stats.predTakenBranches;
        }

        // // Finalize BB and insert in FTQ.
        // DPRINTF(BAC, "[tid:%i] [sn:%llu] BB:%#x end with PC:%#x. Size:%ib "
        //                     "Start next BB at addr:%#x in next cycle\n",
        //         tid, seqNum, curFT, curAddr, curFT->size(),
        //         next_pc->instAddr());

        // // Add the branch instruction to the BB and insert it in FTQ.
        // // curFT->addTerminalBranch(inst);
        // curFT->addTerminal(this_pc, seqNum, true, predict_taken, *next_pc);

    } else {

        // Not a branch therefore we will continue the next FT at the
        // next address
        search_pc->advance();

    }



        // // curFT->taken = predict_taken;
        // // curFT->setPredTarg(this_pc);
        // curFT->addTerminal(this_pc, seqNum, true, predict_taken, *next_pc);

        // inst->setPredControl(true);
        // inst->setPredTarg(this_pc);
        // inst->setPredTaken(predict_taken);



    // Finish fetch target
    curFT->addTerminal(this_pc, curFT->ftNum(), branchFound,
                        predict_taken, *search_pc);

    ftq->insert(tid, curFT);
    wroteToTimeBuffer = true;

    // ftq[tid].push_back(curFT);
    // basicBlockProduce[tid] = NULL;

    if (ftq->isFull(tid)) {
        DPRINTF(BAC, "FTQ full\n");
        bacStatus[tid] = FTQFull;
        status_change = true;
    }

    DPRINTF(BAC, "[tid:%i] [fn:%llu] %i addresses searched. "
                "Branch found:%i. Continue with PC:%s, sz:%i in next cycle\n",
                tid, curFT->ftNum(), numAddrSearched, branchFound,
                *search_pc, search_pc->as<X86ISA::PCState>().size());

    stats.ftSizeDist.sample(numAddrSearched);

    // Finally set the BPU PC to the next FT in the next cycle
    set(this_pc, *search_pc);

    ftq->dumpFTQ(tid);

    // } else {

    // }
}













// BAC::BasicBlockPtr
// BAC::getCurrentFetchTarget(ThreadID tid, bool &status_change)
// {
    // DPRINTF(BAC, "%s:\n",__func__);

    // // If the FTQ is empty wait unit its filled upis available.
    // if (ftq[tid].empty()){

    //     DPRINTF(BAC, "[tid:%i] FTQ is empty\n", tid);
    //     fetchStatus[tid] = FTQEmpty;
    //     status_change = true;
    //     return nullptr;

    // }


    // // Get the first entry in the FTQ.
    // // Don't pop it here. We pop it once done.
    // BasicBlockPtr curFT = ftq[tid].front();

    // DPRINTF(BAC, "[tid:%i] Fetch from %s PC:%#x. FTQ size:%i\n",
    //         tid, curFT->print(), *pc[tid], ftq[tid].size());

    // // Check if the current pc is the current fetch target.
    // // If not BPU and FE have diverged.
    // // Squash and reset BPU
    // if (!curFT->isInBB(pc[tid]->instAddr())) {
    //     DPRINTF(BAC, "[tid:%i] PC:%#x not within new fetch target!\n",
    //         tid, *pc[tid]);
    //     // ftqStatus[tid] = FTQSquash;
    //     // status_change = true;
    // }
    // // update the current PC to the new fetch target.
    // // set(pc[tid], curFT->readStartPC());
    // return curFT;
// }







// DynInstPtr
// BAC::getInstrFromBB(ThreadID tid, StaticInstPtr staticInst,
//             StaticInstPtr curMacroop, const PCStateBase &this_pc,
//             PCStateBase &next_pc)
// {

//     /**
//      * In this function the predecoded static instruction is compared with
//      * the instruction the BP predicted.
//      * The BP only cares about branches. Hence, if its not a branch we
//      * will just build the instruction with the next available sequence number
//      * If this is the last instruction in the
//      *
//      */

//     DPRINTF(BAC, "%s:\n",__func__);

//     // bool status_changed= false;
//     // bb = getCurrentFetchTarget(tid, status_changed);
//     assert(!ftq[tid].empty());


//     // Get the first entry in the FTQ.
//     // Don't pop it here. We pop it once done.
//     BasicBlockPtr bb = ftq[tid].front();

//     DPRINTF(BAC, "[tid:%i] Fetch from %s PC:%#x. FTQ size:%i\n",
//             tid, bb->print(), 0, ftq[tid].size());

//     assert(bb);

//     DPRINTF(BAC, "BB:%s: Range[%#x->%#x] PC:%#x\n", bb, bb->startAddress(), bb->endAddress(), this_pc.instAddr());

//     // Check if we have exceeded the current fetch target otherwise
//     // A new one needs to be poped.
//     if (bb->hasExceeded(this_pc.instAddr())) {
//         DPRINTF(BAC, "Exceed BB:%#x PC:%#x. Get next Fetch Target first\n",
//                 bb, this_pc.instAddr());
//         DPRINTF(BAC, "Done with BB:%#x. Pop from FTQ.\n", bb);
//         ftq[tid].pop_front();
//         // return NULL;
//         // Get the next BB
//         assert(!ftq[tid].empty());
//         bb = ftq[tid].front();
//     }

//     // Check if the current pc is the current fetch target.
//     // If not BPU and FE have diverged.
//     // Squash and reset BPU
//     if (!bb->isInBB(this_pc.instAddr())) {
//         DPRINTF(BAC, "[tid:%i] PC:%#x not within new fetch target!\n",
//             tid, this_pc);
//         doFTQSquash(this_pc, tid);
//         // status_change = tru
//         return NULL;
//     }




// bool reached_end = false;
//     // InstSeqNum seqNum = 0;

//     bool last_uop = staticInst->isLastMicroop();


//     // First check if is a regular instruction or a branch.
//     // Build the dynamic instruction respectively.
//     DPRINTF(BAC, "BB:%s: Range[%#x->%#x] PC:%#x, last uOp:%i\n",
//                 bb, bb->startAddress(), bb->endAddress(), this_pc.instAddr(),
//                 last_uop);


//     if (last_uop && bb->isTerminal(this_pc.instAddr())) {

//         reached_end = true;
//         DPRINTF(BAC, "Reached end of BB:%#x, PC:%#x\n", bb, bb->endAddress());
//         // seqNum = bb->brSeqNum;

//     } else {
//          DPRINTF(BAC, "End of BB:%#x not reached PC:%#x\n", bb, this_pc.instAddr());

//         // assert(!bb->seqNumbers.empty());
//         // Get the next sequence number.
//         // seqNum = bb->getNextSeqNum();
//         // bb->seqNumbers.pop_front();
//     }


//     // ThreadID tid = bb->getTid();




//     // if (bb->isInBB(this_pc.instAddr())) {

//     // } else {

//     // }


//     // if (this_pc.instAddr() < bb->endAddress()) {


//     // } else if (this_pc.instAddr() == bb->endAddress()) {



//     // } else {
//     //     exceed = true;

//     // }


//     // Build instruction with the seq number and the pre decoded information
//     // Insert inst in the instruction queue.
//     // DynInstPtr inst = buildInst(tid, staticInst, curMacroop,
//     //                     this_pc, next_pc, seqNum, true, true);

//     DynInstPtr inst = buildInst(
//                     tid, staticInst, curMacroop, this_pc, next_pc, true);
//     set(next_pc, this_pc);






// //  // Do branch prediction check here.
// //     // A bit of a misnomer...next_PC is actually the current PC until
// //     // this function updates it.
// //     bool predict_taken;

// //     if (!inst->isControl()) {
// //         inst->staticInst->advancePC(next_pc);
// //         inst->setPredControl(false);
// //         inst->setPredTarg(next_pc);
// //         inst->setPredTaken(false);
// //         inst->setResteered(false);
// //         return false;
// //     }
// //     inst->setPredControl(true);

// //     // quick hack
// //     // auto target = inst->branchTarget();

// //     ThreadID tid = inst->threadNumber;
// //     predict_taken = bpu->predict(inst->staticInst, inst->seqNum,
// //                                         next_pc, tid);

// //     if (predict_taken) {
// //         DPRINTF(BAC, "[tid:%i] [sn:%llu] Branch at PC %#x "
// //                 "predicted to be taken to %s\n",
// //                 tid, inst->seqNum, inst->pcState().instAddr(), next_pc);
// //     } else {
// //         DPRINTF(BAC, "[tid:%i] [sn:%llu] Branch at PC %#x "
// //                 "predicted to be not taken\n",
// //                 tid, inst->seqNum, inst->pcState().instAddr());
// //     }

// //     DPRINTF(BAC, "[tid:%i] [sn:%llu] Branch at PC %#x "
// //             "predicted to go to %s\n",
// //             tid, inst->seqNum, inst->pcState().instAddr(), next_pc);
// //     inst->setPredTarg(next_pc);
// //     inst->setPredTaken(predict_taken);
// //     inst->setResteered(false);

// //     ++fetchStats.branches;

// //     if (predict_taken) {
// //         ++fetchStats.predictedBranches;
// //     }

// //     ppFTQInsert->notify(inst);
// //     return predict_taken;










//     // if (reached_end && bb->isTerminalBranch(this_pc.instAddr())) {

//     // Check if the last branch was perdicted as a branch.


//     // If this instruction was a branch
//     if (inst->isControl()) {


//         // bool can_resteer = false;
//         // // Direct unconditional branches can be resteered at this point if
//         // if ((inst->isDirectCtrl() && inst->isUncondCtrl())
//         //     || (inst->isReturn())) {
//         //     can_resteer = true;
//         // }

//         bool was_pred_taken = false;
//         if (reached_end && bb->isTerminalBranch(this_pc.instAddr())) {
//             was_pred_taken = bb->taken;
//         }





//         // The end of the basic block is reached.
//         // Check if the static instruction match with what the BP predicted.

//         // Update the branch after pre-decode
//         bool pred_taken = updatePostFetch(next_pc,
//                                             bb->ftNum(), inst->seqNum,
//                                             staticInst, tid);
//         // bool pred_taken = true;


//         ++bacStats.branches;
//         if (pred_taken) {
//             ++bacStats.predictedBranches;
//         }

//         if (pred_taken) {
//             if (was_pred_taken) {

//                 // No post fetch correction keep the predicted target
//                 set(next_pc, bb->readPredTarg());

//                 DPRINTF(BAC, "[tid:%i] [sn:%llu] Post Fetch PC: %#x "
//                     "no resteer. predTarget:%s\n",
//                     tid, inst->seqNum, inst->pcState().instAddr(),
//                     next_pc);
//             } else {

//                 // Post fetch correction. The BPU already resteered the
//                 // direction.
//                 DPRINTF(BAC, "[tid:%i] [sn:%llu] Post Fetch PC: %#x "
//                         "-> resteered to %s\n",
//                         tid, inst->seqNum, inst->pcState().instAddr(),
//                         next_pc);

//                 doFTQSquash(next_pc, tid);

//             }
//         } else {
//             assert(!was_pred_taken);
//             // Remain taken.
//             inst->staticInst->advancePC(next_pc);
//         }
//         // else {
//         //     DPRINTF(BAC, "[tid:%i] [sn:%llu] Branch at PC %#x "
//         //             "predicted to be not taken\n",
//         //             tid, inst->seqNum, inst->pcState().instAddr());
//         // }


//         inst->setPredTaken(pred_taken);
//         inst->setPredTarg(next_pc);
//         inst->setPredControl(true);


//         DPRINTF(BAC, "[tid:%i] [sn:%llu] Branch at PC %#x "
//                 "predicted to go to %s\n",
//                 tid, inst->seqNum, inst->pcState().instAddr(), next_pc);

//     } else {

//         // The BP predict a regular instruction or
//         // a branch that was not taken in case taken only history is used.
//         // Set the prediction information in respect of that.
//         // Note: At this point we might have also already exceeded the
//         // basic block. However, we will not give the instruction a
//         // second chance for prediction.
//         inst->staticInst->advancePC(next_pc);
//         inst->setPredControl(false);
//         inst->setPredTarg(next_pc);
//         inst->setPredTaken(false);
//     }

//     // If done with BB remove it.
//     if (reached_end) {
//         DPRINTF(BAC, "Done with BB:%#x. Pop from FTQ.\n", bb);
//         ftq[tid].pop_front();
//     }
//     return inst;
// }












/// Branch predictor part ------------------------------------------




bool
BAC::predict(const StaticInstPtr &inst, const FetchTargetPtr &ft,
                   PCStateBase &pc, ThreadID tid)
{

    // TODO:
    assert(bpu != nullptr);

    /** Perform the prediction. */
    BPredUnit::PredictorHistory* bpu_history = nullptr;
    bool taken  = bpu->predict(inst, ft->ftNum(), pc, tid, bpu_history);

    // assert(bpu_history!=nullptr);

    /** Push the record into the history buffer */
    // preFetchPredHist[tid].push_front(bpu_history);
    ft->bpu_history = static_cast<void*>(bpu_history);

    DPRINTF(Branch,"[tid:%i] [ftn:%llu] History added.\n", tid, ft->ftNum());

    return taken;
}


/// Not decoupled function
bool
BAC::predict(const DynInstPtr &inst, const InstSeqNum &seqNum,
                   PCStateBase &pc, ThreadID tid)
{
    ppFTQInsert->notify(inst);
    return bpu->predict(inst->staticInst, inst->seqNum, pc, tid);
}



bool
BAC::updatePostFetch(PCStateBase &pc, const FetchTargetPtr &ft,
                            const InstSeqNum &seqNum,
                            const StaticInstPtr &inst, ThreadID tid)
{

    assert(ft->ftNum() == ftq->readHead(tid)->ftNum());


    BPredUnit::BranchClass brType = getBranchClass(inst);

    /** After pre-decode we know if an instruction
     * was a branch as well as the target of direct branches.
     * Hence, we can resteer unconditional direct branches.
     * Also we can resteer returns.
     * Can we resteer
     */
    bool can_resteer = (inst->isDirectCtrl() && inst->isUncondCtrl())
                        || (bpu->ras && inst->isReturn()) ? true : false;


    DPRINTF(Branch, "[tid:%i] Update pre-decode [ftn:%llu] => [sn:%llu] "
                    "PC:%#x, %s, can resteer:%i, "
                    "FT[br:%i, taken:%i, end:%#x]\n"
                    , tid, ft->ftNum(), seqNum,
                    pc.instAddr(), bpu->toStr(brType),
                    ft->hasBranch(), ft->predtaken(), ft->endAddress());



// Old with preFetchHistory
    // // Iterate from the back.
    // auto hist = preFetchPredHist[tid].end();
    // while (hist != preFetchPredHist[tid].begin()) {
    //     --hist;

    // // for (auto& hist : predHist[tid]) {
    //     if (hist->seqNum == ft->ftNum()) {

    //         DPRINTF(Branch, "[tid:%i] [ft:%llu] hit sn: %s ? %s\n"
    //                 , tid, seqNum,
    //                 toStr(hist->type),
    //                 toStr(brType));
    //         break;
    //     }
    // }

    // if ((hist != preFetchPredHist[tid].begin())
    //     && (hist->type != brType)) {

    //     DPRINTF(Branch, "[tid:%i] [fn:%llu] Update static instr "
    //                     "fail: types dont match!\n", tid, ft->ftNum());
    //     assert(false);
    // }







    bool resteer = false;
    BPredUnit::PredictorHistory* hist = nullptr;

    /** Check if (1) for this fetch target a branch was
      * predicted and (2) if yes if the branches match earch other.
      * Only then we have a valid history already.
      */
    if (ft->hasBranch() && (ft->endAddress() == pc.instAddr())) {
        assert(ft->bpu_history != nullptr);

        // Pop the history from the FTQ to move it later to the
        // history buffer.
        hist = static_cast<BPredUnit::PredictorHistory*>(ft->bpu_history);
        ft->bpu_history = nullptr;

        DPRINTF(Branch, "[tid:%i] Pop history from FTQ: "
                    "PC:%#x. taken:%i, target:%#x\n", tid,
                    hist->pc, hist->predTaken, hist->target);
        // Check if we need/can to resteer
        if (!hist->predTaken && can_resteer) {
            resteer = true;
        }
    } else {
        // No valid history. That means no branch was detected and
        // 'not taken' was assumed.
        // We need to create a history but before that we can resteer.
        if (can_resteer) {
            resteer = true;
        }
    }


    // Do the resteering now if needed. This is bascially going over all
    // fetch targets and reverting all predictions made.
    // For now we do only the branch history squash right away.
    // The actual FTQ clear will be done in the next cycle.
    // squashing by tansition the state: -> squashing -> running
    // If there is no history yet we can revert all predictions
    // otherwise we revert all but the head.
    if (resteer) {
        squashBpuHistories(tid);
        /** Invalidate the FTQ. We then need to wait for a resteer signal */
        ftq->invalidate(tid);
        bacStatus[tid] = Idle;
    }





    // if (hist == preFetchPredHist[tid].begin()) {
    //     DPRINTF(Branch, "[tid:%i] [fn:%llu] No history for PC:%#x, %s\n",
    //             tid, ft->ftNum(), pc, toStr(brType));

    //     // Direct unconditional branches can be resteered at this point if
    //     if ((inst->isDirectCtrl() && inst->isUncondCtrl())
    //         || (bpu->ras && inst->isReturn())) {
    //         resteer = true;
    //     }
    //     hist_valid = false;

    // } else {
    //     if (!hist->predTaken &&
    //         ((inst->isDirectCtrl() && inst->isUncondCtrl())
    //         || (bpu->ras && inst->isReturn()))) {
    //         resteer = true;
    //     }

    //     // Move the history to the real prediction history
    //     bpu->predHist[tid].push_front(*hist);
    //     preFetchPredHist[tid].erase(hist);
    //     hist = bpu->predHist[tid].begin();
    //     hist_valid = true;
    //     DPRINTF(Branch, "[tid:%i] [sn:%llu] Move history to main buffer after pre-decode"
    //             "\n", tid, seqNum);
    //     hist->inst = inst;
    //     hist->seqNum = seqNum;
    // }


    // if (resteer) {
    //     // First resteer then fix history
    //     DPRINTF(Branch, "[tid:%i] [sn:%llu] Squash FTQ\n", tid, seqNum);
    //     // TODO: check the indirect predictor and make it properly
    //     bpu->squash(ft->ftNum(), tid);
    // }



    // We will mess up the indirect predictor if the seq number is
    // smaller than the fetch target number
    // assert(ftNum > seqNum);

    // After squashing the BP history
    // we can create a new history if non exits
    if (hist == nullptr) {
        DPRINTF(Branch,
                "[tid:%i] [sn:%llu] No branch history. "
                "Create pred. history PC %s\n",
                tid, seqNum, pc);
        // Create a dummy PC with the current one
        // we dont care what the BP is predicting as
        // we fix it anyway in a moment.

        // Create the history.
        // Because it is a dummy prediction its always not taken.
        //
        hist = new BPredUnit::PredictorHistory(seqNum, pc.instAddr(),
                                        false, NULL, NULL, NULL,
                                        tid, brType, inst);

        if (bpu->iPred) {
            bpu->iPred->genIndirectInfo(tid, hist->indirectHistory);
        }


        if (inst->isUncondCtrl()) {
            DPRINTF(Branch, "[tid:%i] [sn:%llu] Unconditional control\n",
                tid,seqNum);
            // Tell the BP there was an unconditional branch.
            bpu->uncondBranch(tid, pc.instAddr(), hist->bpHistory);
        } else {
            // ++stats.condPredicted;
            // We can only resteer unconditional branches.
            // but we make a dummy prediction and delete change it to not
            // taken right away.
            bpu->lookup(tid, pc.instAddr(), hist->bpHistory);
            bpu->btbUpdate(tid, pc.instAddr(), hist->bpHistory);
        }

        /** A special case are indirect calls. For those we do not have
         * a target so we cannot resteer. But we need to push the address
         * on the stack.
         */
        if (bpu->ras && inst->isCall() && inst->isIndirectCtrl()) {
            assert(!resteer);
            bpu->ras->push(tid, pc, hist->rasHistory);
            hist->wasCall = true;

            DPRINTF(Branch, "[tid:%i] [sn:%llu] Instruction %s was "
                            "a indirect call, adding to the RAS\n",
                            tid, seqNum, pc);
        }

    }

    assert(hist != nullptr);
    DPRINTF(Branch, "[tid:%i] [sn:%llu] Move history to main buffer after\n",
                    tid, seqNum);
    hist->inst = inst;
    hist->seqNum = seqNum;
    bpu->predHist[tid].push_front(hist);


    // We are done if we don't need to resteer.
    if (!resteer) {
        return hist->predTaken;
    }


    /** Early resteering. */
    std::unique_ptr<PCStateBase> target(pc.clone());

    // We can fix the RAS
    if (bpu->ras && inst->isReturn()) {
        hist->wasReturn = true;

        // If it's a function return call, then look up the address
        // in the RAS.
        const PCStateBase *ras_top = bpu->ras->pop(tid, hist->rasHistory);
        if (ras_top) {
            set(target, inst->buildRetPC(pc, *ras_top));
            // inst->buildPC()
            hist->usedRAS = true;

            DPRINTF(Branch, "[tid:%i] [sn:%llu] Instruction %s is a "
                "return, RAS predicted target: %s\n",
                tid, seqNum, pc, *target);
        }
    }

    // Record calls in the RAS.
    if (bpu->ras && inst->isCall()) {

        bpu->ras->push(tid, pc, hist->rasHistory);
        hist->wasCall = true;

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

    hist->target = target->instAddr();
    hist->predTaken = true;
    set(pc, *target);


    // predHist[tid].push_front(hist);

    // DPRINTF(Branch,
    //         "[tid:%i] [sn:%llu] History entry added. "
    //         "predHist.size(): %i\n",
    //         tid, seqNum, predHist[tid].size());
    return hist->predTaken;
}

void
BAC::profileCycle(ThreadID tid)
{
    switch (bacStatus[tid]) {
    case Idle:
        stats.idleCycles++;
        break;
    case Running:
        stats.runCycles++;
        break;
    case Squashing:
        stats.squashCycles++;
        break;
    case FTQFull:
        stats.ftqFullCycles++;
        break;

    default:
        break;
    }
}


BAC::BACStats::BACStats(o3::CPU *cpu, BAC *bac)
                                        // BAC *decBpu)

    : statistics::Group(cpu, "bac"),

    ADD_STAT(idleCycles, statistics::units::Cycle::get(),
            "Number of cycles BAC is idle. (PC invalid)"),
    ADD_STAT(runCycles, statistics::units::Cycle::get(),
            "Number of cycles BAC is running"),
    ADD_STAT(squashCycles, statistics::units::Cycle::get(),
            "Number of cycles BAC is squashing"),
    ADD_STAT(ftqFullCycles, statistics::units::Cycle::get(),
            "Number of cycles BAC has spent waiting for FTQ to become free"),
    ADD_STAT(fetchTargetsCreated, statistics::units::Count::get(),
            "Number of fetch targets created "),
    ADD_STAT(branches, statistics::units::Count::get(),
            "Number of branches that BAC encountered"),
    ADD_STAT(predTakenBranches, statistics::units::Count::get(),
            "Number of branches that BAC predicted taken."),
    ADD_STAT(branchMisspredict, statistics::units::Count::get(),
            "Number of branches that BAC has predicted taken"),
    ADD_STAT(noBranchMisspredict, statistics::units::Count::get(),
            "Number of branches that BAC has predicted taken"),
    ADD_STAT(ftSizeDist, statistics::units::Count::get(),
             "Number of bytes per fetch target")
{
    using namespace statistics;

    ftSizeDist
        .init(/* base value */ 0,
            /* last value */ bac->fetchTargetWidth,
            /* bucket size */ 1)
        .flags(statistics::pdf);
}

} // namespace branch_prediction
} // namespace gem5
