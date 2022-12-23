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


#ifndef __CPU_O3_BAC_HH__
#define __CPU_O3_BAC_HH__

#include <deque>

// #include "cpu/o3/cpu.hh"
#include "base/statistics.hh"
#include "cpu/o3/comm.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
// #include "cpu/o3/ftq.hh" ///// Remove from header
#include "cpu/o3/limits.hh"
#include "cpu/pred/bpred_unit.hh"
#include "cpu/timebuf.hh"



namespace gem5
{

struct BaseO3CPUParams;

namespace o3
{

class CPU;
class FTQ;
class FetchTarget;
typedef std::shared_ptr<FetchTarget> FetchTargetPtr;


/**
 * TODO: Maybe better naming would BAC: Branch and Address Calculation.
 * The Branch Predict stage handles branch prediction and next PC
 * address calculation.
 */
class BAC
{

  public:
    /** Overall decoupled BPU stage status. Used to determine if the CPU can
     * deschedule itself due to a lack of activity.
     */
    enum BACStatus
    {
        Active,
        Inactive
    };

    /** Individual thread status. */
    enum ThreadStatus
    {
        Idle,
        Running,
        Squashing,
        FTQFull
    };

    // enum FTQStatus
    // {
    //     FTQActive,
    //     FTQSquash,
    //     FTQFull,
    //     FTQInactive
    // };

  private:
    /** Decode status. */
    BACStatus _status;

    /** Per-thread status. */
    ThreadStatus bacStatus[MaxThreads];
    // FTQStatus ftqStatus[MaxThreads];

  public:
    ProbePointArg<DynInstPtr> *ppFTQInsert;


    // /**
    //  * @param params The params object, that has the size of the BP and BTB.
    //  */
    // // BAC(const BACParams &params);
    // ~BAC() = default;

  public:
    /** BAC constructor. */
    BAC(CPU *_cpu, const BaseO3CPUParams &params);


    // Interfaces to CPU ----------------------------------

    /** Returns the name of the stage. */
    std::string name() const;

    /** Registers probes. */
    void regProbePoints();

    /** Sets the main backwards communication time buffer pointer. */
    void setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr);

    /** Sets pointer to time buffer used to communicate to the next stage. */
    // void setFetchTargeQueue(TimeBuffer<FTQStruct> *ftq_ptr);

    // /** Sets pointer to time buffer coming from fetch. */
    // void setFetchQueue(TimeBuffer<FetchStruct> *fq_ptr);

    /** Sets pointer to list of active threads. */
    void setActiveThreads(std::list<ThreadID> *at_ptr);

    /** Initialize stage. */
    void setFetchTargetQueue(FTQ * _ptr);

    /** Initialize stage. */
    void startupStage();

    /** Clear all thread-specific states*/
    void clearStates(ThreadID tid);

    /** Resume after a drain. */
    void drainResume();

    /** Perform sanity checks after a drain. */
    void drainSanityCheck() const;

    /** Has the stage drained? */
    bool isDrained() const;

    /**
     * Stall the fetch stage after reaching a safe drain point.
     *
     * The CPU uses this method to stop fetching instructions from a
     * thread that has been drained. The drain stall is different from
     * all other stalls in that it is signaled instantly from the
     * commit stage (without the normal communication delay) when it
     * has reached a safe point to drain from.
     */
    void drainStall(ThreadID tid);

    /** Takes over from another CPU's thread. */
    void takeOverFrom() { resetStage(); }


    /** For priority-based fetch policies, need to keep update priorityList */
    void deactivateThread(ThreadID tid);
  private:
    /** Reset this pipeline stage */
    void resetStage();

    /** Changes the status of this stage to active, and indicates this
     * to the CPU.
     */
    void switchToActive();

    /** Changes the status of this stage to inactive, and indicates
     * this to the CPU.
     */
    void switchToInactive();

    /** Checks if a thread is stalled. */
    bool checkStall(ThreadID tid) const;

    /** Updates overall BAC stage status; to be called at the end of each
     * cycle. */
    void updateBACStatus();

    /** Checks all input signals and updates the status as necessary.
     *  @return: Returns if the status has changed due to input signals.
     */
    bool checkSignalsAndUpdate(ThreadID tid);

  public:
    /** Ticks ftq, processing all input signals and create the next fetch
     * target.
     */
    void tick();



















  /** BRANCH PRedictor ===============================================*/

    /**
     * Tells the branch predictor to commit any updates until the given
     * sequence number.
     * @param done_sn The sequence number to commit any older updates up until.
     * @param tid The thread id.
     */
    void update(const InstSeqNum &done_sn, ThreadID tid)
    { bpu->update(done_sn, tid); }

    /**
     * Squashes all outstanding updates until a given sequence number.
     * @param squashed_sn The sequence number to squash any younger updates up
     * until.
     * @param tid The thread id.
     */
    void squash(const InstSeqNum &squashed_sn, ThreadID tid)
    { bpu->squash(squashed_sn, tid); }

    /**
     * Squashes all outstanding updates until a given sequence number, and
     * corrects that sn's update with the proper address and taken/not taken.
     * @param squashed_sn The sequence number to squash any younger updates up
     * until.
     * @param corr_target The correct branch target.
     * @param actually_taken The correct branch direction.
     * @param tid The thread id.
     * @param inst The static instruction that caused the misprediction
     */
    void squash(const InstSeqNum &squashed_sn, const PCStateBase &corr_target,
                bool actually_taken, ThreadID tid)
    { bpu->squash(squashed_sn, corr_target, actually_taken, tid); }


    /**
     * Predicts whether or not the instruction is a taken branch, and the
     * target of the branch if it is taken.
     * @param inst The branch instruction.
     * @param PC The predicted PC is passed back through this parameter.
     * @param tid The thread id.
     * @return Returns if the branch is taken or not.
     */
    bool predict(const DynInstPtr &inst, const InstSeqNum &seqNum,
                 PCStateBase &pc, ThreadID tid);

    bool predict(const StaticInstPtr &inst, const FetchTargetPtr &ft,
                 PCStateBase &pc, ThreadID tid);

    /**
     * Post fetch update/correction ----------------------
     * After pre-decoding instructions the type of a branch as well as the
     * target for direct branches in known.
     *
     * Together with inserting an instruction into the instruction queue
     *  instruction matches the predicted
     * instruction type. If so update the information with the new.
     * In case the types dont match something is wrong and we need
     * to squash. (should not be the case.)
     * @param seq_num The branches sequence that we want to update.
     * @param inst The new pre-decoded branch instruction.
     * @param tid The thread id.
     * @return Returns if the update was successful.
     */
    bool updatePostFetch(PCStateBase &pc, const FetchTargetPtr &ft,
                          const InstSeqNum &seqNum,
                          const StaticInstPtr &inst, ThreadID tid);


// /**
//      * Need to instantiate all pure virtual function from BPredUnit
//      * None are used in this bpred.
//      */
//     void
//     uncondBranch(ThreadID tid, Addr pc, void *&bp_history) override
//     {}
//     // void
//     // squash(ThreadID tid, void *bp_history) override
//     // {}
//     bool
//     lookup(ThreadID tid, Addr instPC, void *&bp_history) override
//     {
//         return false;
//     }
//     void
//     btbUpdate(ThreadID tid, Addr instPC, void *&bp_history) override
//     {}
//     void
//     update(ThreadID tid, Addr instPC, bool taken, void *bp_history,
//             bool squashed, const StaticInstPtr &inst, Addr corrTarget) override
//     {}


  private:

    typedef std::deque<branch_prediction::BPredUnit::PredictorHistory*> History;

    std::vector<History> preFetchPredHist;


    branch_prediction::BPredUnit::BranchClass getBranchClass(StaticInstPtr inst)
    {
      return bpu->getBranchClass(inst);
    }

    std::string toStr(branch_prediction::BPredUnit::BranchClass type) const
    {
        return std::string(enums::BranchClassStrings[type]);
    }












    /********************************************************************
     *
     * Decoupled Frontend functionality
     *
     * In a decoupled frontend the Branch predictor Unit
     * is not directly queried by Fetch once it pre-decodes
     * a branch. Instead BPU and Fetch is separated and
     * connected via a queue fetch target queue (FTQ). The BPU generates
     * fetch targets (basic block addresses (BB)) and inserts them in
     * the queue. Fetch will consume the addresses and read them from the
     * I-cache.
     * The advantages are that it (1) cuts the critical path and (2)
     * allow a precise, BPU guided prefetching of the fetch targets.
     *
     * For determining next PC addresses the BPU relies on the BTB.
     *
     *
     *
     *******************************************************************/
    //

  private:
    /** Unique sequence number for fetch targets */
    InstSeqNum globalFTSeqNum;
    InstSeqNum getAndIncrementFTSeq() { return globalFTSeqNum++; }

    FetchTargetPtr newFetchTarget(ThreadID tid, const PCStateBase &start_pc);

    /** Main function that feeds the FTQ with new fetch targets.
      * By leveraging the BTB up to N consecutive addresses are searched
      * to detect a branch instruction.
      * For every BTB hit the direction predictor is asked to make a
      * prediction.
      * In every cycle one fetch target is created. A fetch target ends
      * once the first branch instruction is detected or the maximum
      * search bandwidth for a cycle is reached.
      *
    performs the actual search through

    Feed the fetch target queue. */
    void generateFetchTargets(ThreadID tid, bool &status_change);









    // FetchTargetPtr getCurrentFetchTarget(ThreadID tid, bool &status_change);

    /** The decoupled PC used by the BPU to generate fetch targets
     *
     */
    class BpuPCState : public GenericISA::UPCState<1>
    {
      protected:
        using Base = GenericISA::UPCState<1>;
        // uint8_t _size;

      public:
        // BpuPCState(const BpuPCState &other) : Base(other), _size(other._size) {}
        BpuPCState(const BpuPCState &other) : Base(other) {}

        BpuPCState &operator=(const BpuPCState &other) = default;
        BpuPCState() {}
        explicit BpuPCState(Addr val) { set(val); }
        void advance() override
        {
            Base::advance();
        }
        void
        update(const PCStateBase &other) override
        {
            Base::update(other);
        }
    };

    // using BpuPCState = GenericISA::UPCState<1>;
    typedef std::unique_ptr<BpuPCState> BpuPCStatePtr;


    // Addr decoupledOffset[MaxThreads];

    // DynInstPtr buildInstPlaceholder(ThreadID tid, StaticInstPtr staticInst,
    //                         const PCStateBase &this_pc);

    // DynInstPtr fillInstPlaceholder(const DynInstPtr &src, StaticInstPtr staticInst,
    //     StaticInstPtr curMacroop, const PCStateBase &this_pc,
    //     const PCStateBase &next_pc, bool trace);


    // DynInstPtr getInstrFromBB(ThreadID tid, StaticInstPtr staticInst,
    //         StaticInstPtr curMacroop, const PCStateBase &this_pc,
    //         PCStateBase &next_pc);

    /** Squashes BAC for a specific thread and resets the PC. */
    void squash(const PCStateBase &new_pc, ThreadID tid);

    /** Squashes the BPU histories in the FTQ.
      * by iterating from tail to head and reverts the predictions made.
      * @param squash_head whether the head fetch target should be squashed
      */
    void squashBpuHistories(ThreadID tid);

    /** Update the stats per cycle */
    void profileCycle(ThreadID tid);


  public:



  private:
    /** Pointer to the main CPU. */
    CPU* cpu;

    /** BPredUnit. */
    branch_prediction::BPredUnit* bpu;

    /** Fetch target Queue. */
    FTQ* ftq;

    /** Time buffer interface. */
    TimeBuffer<TimeStruct> *timeBuffer;

    /** Wire to get fetches's information from backwards time buffer. */
    TimeBuffer<TimeStruct>::wire fromFetch;

    /** Wire to get decode's information from backwards time buffer. */
    TimeBuffer<TimeStruct>::wire fromDecode;

    /** Wire to get rename's information from backwards time buffer. */
    TimeBuffer<TimeStruct>::wire fromRename;

    /** Wire to get iew's information from backwards time buffer. */
    TimeBuffer<TimeStruct>::wire fromIEW;

    /** Wire to get commit's information from backwards time buffer. */
    TimeBuffer<TimeStruct>::wire fromCommit;

    /** Wire used to write any information heading to fetch. */
    TimeBuffer<FetchStruct>::wire toFetch;

    /** The decoupled PC which runs ahead of fetch */
    // TODO: rename
    std::unique_ptr<PCStateBase> pc[MaxThreads];


    /** Variable that tracks if BAC has written to the time buffer this
     * cycle. Used to tell CPU if there is activity this cycle.
     */
    bool wroteToTimeBuffer;

    /** Source of possible stalls. */
    struct Stalls
    {
        bool fetch;
        bool drain;
        bool bpu;
    };

    /** Tracks which stages are telling the ftq to stall. */
    Stalls stalls[MaxThreads];

    /** Fetch to BAC delay. */
    const Cycles fetchToBacDelay;

    /** Decode to fetch delay. (Same delay for BAC) */
    const Cycles decodeToFetchDelay;

    /** Rename to fetch delay. (Same delay for BAC) */
    const Cycles renameToFetchDelay;

    /** IEW to fetch delay. (Same delay for BAC) */
    const Cycles iewToFetchDelay;

    /** Commit to fetch delay. (Same delay for BAC) */
    const Cycles commitToFetchDelay;

    /** BAC to fetch delay. */
    const Cycles bacToFetchDelay;

    /** The size of the fetch target queue */
    const unsigned ftqSize;

    /** The maximum with of a fetch target. This also determin the
     * maximum search width of the branch predictor in on cycle. */
    const unsigned fetchTargetWidth;

    /** List of Active FTQ Threads */
    std::list<ThreadID> *activeThreads;

    /** Number of threads. */
    const ThreadID numThreads;


  protected:
    struct BACStats : public statistics::Group
    {
      BACStats(CPU *cpu, BAC *bac);

      /** Stat for total number of idle cycles. */
      statistics::Scalar idleCycles;
      /** Stat for total number of normal running cycles. */
      statistics::Scalar runCycles;
      /** Stat for total number of squashing cycles. */
      statistics::Scalar squashCycles;
      /** Stat for total number of cycles the FTQ was full. */
      statistics::Scalar ftqFullCycles;

      /** Stat for total number fetch targets created. */
      statistics::Scalar fetchTargetsCreated;
      /** Total number of branches detected. */
      statistics::Scalar branches;
      /** Total number of branches predicted taken. */
      statistics::Scalar predTakenBranches;

      /** Stat for total number of misspredicted instructions. */
      statistics::Scalar branchMisspredict;
      statistics::Scalar noBranchMisspredict;

      /** Distribution of number of bytes per fetch target. */
      statistics::Distribution ftSizeDist;

    } stats;
    /** @} */
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_BAC_HH__
