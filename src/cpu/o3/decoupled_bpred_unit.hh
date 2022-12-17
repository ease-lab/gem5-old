/*
 * Copyright (c) 2022 The University of Edinburgh
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

#ifndef __CPU_O3_BPREDICT_HH__
#define __CPU_O3_BPREDICT_HH__

#include <deque>

// #include "cpu/o3/cpu.hh"
#include "base/statistics.hh"
#include "cpu/o3/comm.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/limits.hh"
#include "cpu/pred/bpred_unit.hh"
#include "cpu/timebuf.hh"

namespace gem5
{

struct BaseO3CPUParams;

namespace o3
{

class CPU;

/**
 * TODO: Maybe better naming would BAC: Branch and Address Calculation.
 * The Branch Predict stage handles branch prediction and next PC
 * address calculation.
 */
class BPredict
{

  public:
    /** Overall decoupled BPU stage status. Used to determine if the CPU can
     * deschedule itself due to a lack of activity.
     */
    enum FtqStageStatus
    {
        Active,
        Inactive
    };

    /** Individual thread status. */
    enum ThreadStatus
    {
        Running,
        Idle,
        StartSquash,
        Squashing,
        FTQEmpty,
        Blocked,
        Unblocking
    };

    enum FTQStatus
    {
        FTQActive,
        FTQSquash,
        FTQFull,
        FTQInactive
    };

  private:
    /** Decode status. */
    FtqStageStatus _status;

    /** Per-thread status. */
    ThreadStatus thstatus[MaxThreads];
    FTQStatus ftqStatus[MaxThreads];


    ProbePointArg<DynInstPtr> *ppFTQInsert;


    // /**
    //  * @param params The params object, that has the size of the BP and BTB.
    //  */
    // // BPredict(const BPredictParams &params);
    // ~BPredict() = default;

  public:
    /** BPredict constructor. */
    BPredict(CPU *_cpu, const BaseO3CPUParams &params);


    // Interfaces to CPU ----------------------------------
    void startupStage();

    /** Clear all thread-specific states */
    void clearStates(ThreadID tid);


    /** Returns the name of decode. */
    std::string name() const;

    /** Sets the main backwards communication time buffer pointer. */
    void setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr);

    // /** Sets pointer to time buffer used to communicate to the next stage. */
    // void setDecodeQueue(TimeBuffer<DecodeStruct> *dq_ptr);

    // /** Sets pointer to time buffer coming from fetch. */
    // void setFetchQueue(TimeBuffer<FetchStruct> *fq_ptr);

    /** Sets pointer to list of active threads. */
    void setActiveThreads(std::list<ThreadID> *at_ptr);

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

    // /** Takes over from another CPU's thread. */
    // void takeOverFrom() { resetStage(); }


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


  public:
    /** Ticks ftq, processing all input signals and create the next fetch
     * target.
     */
    void tick();





    /**
     * Predicts whether or not the instruction is a taken branch, and the
     * target of the branch if it is taken.
     * @param inst The branch instruction.
     * @param PC The predicted PC is passed back through this parameter.
     * @param tid The thread id.
     * @return Returns if the branch is taken or not.
     */
    bool predict(const StaticInstPtr &inst, const InstSeqNum &seqNum,
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
    bool updatePostFetch(PCStateBase &pc, const InstSeqNum &ftNum,
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
  void setBranchPredictor(branch_prediction::BPredUnit* _bpu) { bpu = _bpu; }


  private:

    typedef std::deque<branch_prediction::BPredUnit::PredictorHistory*> History;

    CPU* cpu;

    branch_prediction::BPredUnit* bpu;

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

    class BasicBlock
    {
      public:
        BasicBlock(ThreadID _tid, const PCStateBase &_start_pc, InstSeqNum seqNum);
        // ~BasicBlock();
      private:
        /* Start address of the basic block */
        std::unique_ptr<PCStateBase> startPC;

        /* End address of the basic block */
        std::unique_ptr<PCStateBase> endPC;

        /** The thread id. */
        const ThreadID tid;

      public:
        /* Start address of the basic block */
        Addr startAddress() { return startPC->instAddr(); }

        /* End address of the basic block */
        Addr endAddress() { return (endPC) ? endPC->instAddr() : MaxAddr; }

        /* Basic Block size */
        unsigned size() { return endAddress() - startAddress(); }

        bool isInBB(Addr addr) {
            return addr >= startAddress() && addr <= endAddress();
        }

        bool isTerminal(Addr addr) {
            return addr == endAddress();
        }

        bool isTerminalBranch(Addr addr) {
            return (addr == endAddress()) && is_branch;
        }

        bool hasExceeded(Addr addr) {
            return addr > endAddress();
        }



        ThreadID getTid() { return tid; }

        /* List of sequence numbers created for the BB */
        std::list<InstSeqNum> seqNumbers;
        InstSeqNum startSeqNum;
        unsigned seq_num_iter = 0;
        InstSeqNum brSeqNum;
        InstSeqNum ftSeqNum;


        // /** The branch that terminate the BB */
        // DynInstPtr terminalBranch;

        // std::unique_ptr<PCStateBase> predPC;
        std::unique_ptr<PCStateBase> predPC;
        /** Set/Read the predicted target of the terminal branch. */
        void setPredTarg(const PCStateBase &pred_pc) { set(predPC, pred_pc); }
        const PCStateBase &readPredTarg() { return *predPC; }

        const PCStateBase &readStartPC() { return *startPC; }
        const PCStateBase &readEndPC() { return *endPC; }

        /** Whether the determining instruction is a branch
         *  In case its a branch if it is taken or not.*/
        bool is_branch;
        bool taken;

        // std::unique_ptr<PCStateBase> targetAddr;

        void addSeqNum(InstSeqNum seqNum) { seqNumbers.push_back(seqNum); }

        InstSeqNum getNextSeqNum() {
            seq_num_iter++;
            assert((startSeqNum + seq_num_iter) < brSeqNum);
            return startSeqNum + seq_num_iter;
        }

        void addTerminal(const PCStateBase &br_pc, InstSeqNum seq,
                          bool _is_branch, bool pred_taken,
                          const PCStateBase &pred_pc);
        void addTerminalNoBranch(const PCStateBase &br_pc, InstSeqNum seq,
                               bool pred_taken, const PCStateBase &pred_pc);

        std::string print() {
          std::stringstream ss;
          ss << "BB[sn:" << startSeqNum << "]: "
             << *startPC << "->" << *endPC << "";
          return ss.str();
        }
    };

    struct FetchTarget
    {
        /* Start address of the basic block */
        std::unique_ptr<PCStateBase> bbStartAddress;

        /* End address of the basic block */
        std::unique_ptr<PCStateBase> bbEndAddress;

        /* Basic Block size */
        unsigned bbSize;

        /** The thread id. */
        ThreadID tid;

        /* Whether the determining branch is taken or not.*/
        bool taken;
        std::unique_ptr<PCStateBase> targetAddr;

      // FetchTarget(Addr _addr = 0, bool
      //     _control = false, bool _taken = false)
      //   : addr(_addr), control(_control), taken(_taken) {};
      // FetchTarget(const FetchTarget &other)
      //   : addr(other.addr), control(other.control), taken(other.taken) {};
    };



    typedef std::shared_ptr<BasicBlock> BasicBlockPtr;
    // using BasicBlockPtr = BasicBlock*;

   typedef std::deque<BasicBlockPtr> FetchTargetQueue;

   FetchTargetQueue ftq[MaxThreads];
    const unsigned ftqSize;

    void dumpFTQ(ThreadID tid);
    bool updateFTQStatus(ThreadID tid);


    BasicBlockPtr basicBlockProduce[MaxThreads];
    BasicBlockPtr basicBlockConsume[MaxThreads];

// #ifdef FDIP
    bool ftqValid(ThreadID tid, bool &status_change) {

        // If the FTQ is empty wait unit its filled upis available.
        // Need at least two cycles for now.
        if (ftq[tid].empty()) {
            // DPRINTF(Fetch, "[tid:%i] FTQ is empty\n", tid);
            thstatus[tid] = FTQEmpty;
            status_change = true;
            return false;
        }
        return true;
    }
// #else
//   bool ftqValid(ThreadID tid, bool &status_change) {return true;}
// #endif

    bool inFTQHead(ThreadID tid, Addr pc) {
        if (ftq[tid].empty()) {
            return false;
        }

      if (!ftq[tid].front()->isInBB(pc)) {
          // DPRINTF(Fetch, "[tid:%i] PC:%#x not within FT!\n",
          //     tid, pc);
          return false;
      }
        return true;
    }

    InstSeqNum globalFTSeqNum;
    InstSeqNum getAndIncrementFTSeq() { return globalFTSeqNum++; }


    /** Feed the fetch target queue.
     */
    void produceFetchTargets(bool &status_change);
    BasicBlockPtr getCurrentFetchTarget(ThreadID tid, bool &status_change);

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

    std::unique_ptr<PCStateBase> bpuPC[MaxThreads];

    // Addr decoupledOffset[MaxThreads];

    DynInstPtr buildInstPlaceholder(ThreadID tid, StaticInstPtr staticInst,
                            const PCStateBase &this_pc);

    DynInstPtr fillInstPlaceholder(const DynInstPtr &src, StaticInstPtr staticInst,
        StaticInstPtr curMacroop, const PCStateBase &this_pc,
        const PCStateBase &next_pc, bool trace);


    DynInstPtr getInstrFromBB(ThreadID tid, StaticInstPtr staticInst,
            StaticInstPtr curMacroop, const PCStateBase &this_pc,
            PCStateBase &next_pc);










    /** Squashes the FTQ for a specific thread and resets the PC. */
    void doFTQSquash(const PCStateBase &new_pc, ThreadID tid);

    /** Checks if a thread is stalled. */
    bool checkStall(ThreadID tid) const;



    /** Source of possible stalls. */
    struct Stalls
    {
        bool fetch;
        bool drain;
    };

    /** Tracks which stages are telling the ftq to stall. */
    Stalls stalls[MaxThreads];











  protected:
    struct BPredictStats : public statistics::Group
    {
      BPredictStats(CPU *cpu, BPredict *decBpu);
        // BPredictStats(statistics::Group *parent);

        statistics::Scalar fetchTargetsCreated;

    } decoupedStats;
    /** @} */
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_BPREDICT_HH__
