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


#ifndef __CPU_O3_FTQ_HH__
#define __CPU_O3_FTQ_HH__

#include <string>
#include <utility>
#include <vector>

#include "arch/generic/pcstate.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "config/the_isa.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/limits.hh"
// #include "cpu/reg_class.hh"
// #include "enums/SMTQueuePolicy.hh"

#define FDIP
namespace gem5
{

struct BaseO3CPUParams;

namespace o3
{

class CPU;

struct DerivO3CPUParams;

class FetchTarget
{
  public:
    FetchTarget(ThreadID _tid, const PCStateBase &_start_pc, InstSeqNum seqNum);
    // ~FetchTarget();
  private:
    /** Start address of the fetch target */
    std::unique_ptr<PCStateBase> startPC;
    Addr _start_addr;

    /** End address of the fetch target */
    std::unique_ptr<PCStateBase> endPC;
    Addr _end_addr;

    /** Predicted target address of the fetch target.
     *  Only valid when the ft ends with branch. */
    Addr _pred_addr;

    /** The thread id. */
    const ThreadID tid;

    /* Fetch targets sequence number */
    const InstSeqNum ftSeqNum;


  public:
    /* Start address of the basic block */
    Addr startAddress() { return startPC->instAddr(); }

    /* End address of the basic block */
    Addr endAddress() { return (endPC) ? endPC->instAddr() : MaxAddr; }

    /* Fetch Target size (number of bytes) */
    unsigned size() { return endAddress() - startAddress(); }

    bool inRange(Addr addr) {
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

    InstSeqNum ftNum() { return ftSeqNum; }


    /** ancore point to attach a branch predictor history.
     * Will carry information while FT is waiting in th FTQ.
    */
    void* bpu_history;


    // /** The branch that terminate the BB */
    // DynInstPtr terminalBranch;

    // std::unique_ptr<PCStateBase> predPC;
    std::unique_ptr<PCStateBase> predPC;
    /** Set/Read the predicted target of the terminal branch. */
    void setPredTarg(const PCStateBase &pred_pc) { set(predPC, pred_pc); }
    const PCStateBase &readPredTarg() { return *predPC; }
    // const Addr readPredTarg() { return _pred_addr; }


    const PCStateBase &readStartPC() { return *startPC; }
    const PCStateBase &readEndPC() { return *endPC; }

    /** Whether the determining instruction is a branch
     *  In case its a branch if it is taken or not.*/
    bool is_branch;

    bool taken;



  public:

    bool hasBranch() { return is_branch; }
    // bool branchAddr() { return endPC->instAddr(); }
    bool predtaken() { return taken; }


    void addTerminal(const PCStateBase &br_pc, InstSeqNum seq,
                      bool _is_branch, bool pred_taken,
                      const PCStateBase &pred_pc);
    // void addTerminalNoBranch(const PCStateBase &br_pc, InstSeqNum seq,
    //                         bool pred_taken, const PCStateBase &pred_pc);

    std::string print();
};




typedef std::shared_ptr<FetchTarget> FetchTargetPtr;




/**
 * FTQ class.
 */
class FTQ
{
  public:
    // typedef typename std::list<FetchTargetPtr>::iterator FetchTargetIt;

    /** Possible FTQ statuses. */
    enum Status
    {
        Invalid,
        Valid,
        Full
    };

  private:
    /** Per-thread FTQ status. */
    Status ftqStatus[MaxThreads];

    // /** FTQ resource sharing policy for SMT mode. */
    // SMTQueuePolicy ftqPolicy;

  public:
    /** FTQ constructor.
     *  @param _cpu   The cpu object pointer.
     *  @param params The cpu params including several FTQ-specific parameters.
     */
    FTQ(CPU *_cpu, const BaseO3CPUParams &params);

    std::string name() const;

    // /** Sets pointer to the list of active threads.
    //  *  @param at_ptr Pointer to the list of active threads.
    //  */
    // void setActiveThreads(std::list<ThreadID> *at_ptr);

    /** Perform sanity checks after a drain. */
    void drainSanityCheck() const;

    /** Has the stage drained? */
    bool isDrained() const;

    /** Takes over another CPU's thread. */
    void takeOverFrom();
















/// The Entry


  public:

    // using FetchTargetPtr = FetchTarget*;

   typedef std::deque<FetchTargetPtr> FetchTargetQueue;
   typedef typename std::list<FetchTargetPtr>::iterator FetchTargetIt;


    void dumpFTQ(ThreadID tid);
    bool updateFTQStatus(ThreadID tid);







////////////////////////////////



    /** Writes a Fetch Target into the FTQ
     *  the FTQ.
     *  @param fetchTarget that should be inserted into the FTQ.
     */
    void insert(ThreadID tid, FetchTargetPtr fetchTarget);

    /** Returns a pointer to the head instruction of a specific thread within
     *  the FTQ.
     *  @return Pointer to the DynInst that is at the head of the FTQ.
     */
    FetchTargetPtr readHead(ThreadID tid);

    /** Checks if an given PC is in FTQ head */
    bool isInHead(ThreadID tid, Addr pc);

    /** Is the head entry ready for the fetch stage. */
    bool isHeadReady(ThreadID tid);

    /** Updates the head fetch target once its fully processed */
    void updateHead(ThreadID tid);




    // /** Returns the number of total free entries in the FTQ. */
    // unsigned numFreeEntries();

    /** Returns the number of free entries in a specific FTQ paritition. */
    unsigned numFreeEntries(ThreadID tid);

    /** Returns the size of the ftq for a specific partition*/
    unsigned size(ThreadID tid);

    /** Returns if a specific thread's queue is full. */
    bool isFull(ThreadID tid);

    /** Returns if the FTQ is empty. */
    bool isEmpty() const;

    /** Returns if a specific thread's queue is empty. */
    bool isEmpty(ThreadID tid) const;

    /** Returns an iterator pointing to the last (most recent) fetch target */
    FetchTargetIt tail(ThreadID tid);
    /** Returns an iterator pointing to one after the last (most recent)
     * fetch target. Hence, is always invalid */
    FetchTargetIt end(ThreadID tid);


    /** Returns an iterator pointing to the first (least recent) FT */
    FetchTargetIt head(ThreadID tid);
    FetchTargetIt begin(ThreadID tid);

    /** Executes the squash, marking squashed fetch targets. */
    void doSquash(ThreadID tid);

    /** Squashes all instructions younger than the given sequence number for
     *  the specific thread.
     */
    void squash(ThreadID tid);

    /***/
    void squashSanityCheck(ThreadID tid);

    /** Set the FTQ to invalide state. Requires squash to recover. */
    void invalidate(ThreadID tid);

    /** Check if the FTQ is invalid and requires squash. */
    bool isValid(ThreadID tid);

    /** Checks if the FTQ is still in the process of squashing instructions.
     *  @retval Whether or not the FTQ is done squashing.
     */
    bool isDoneSquashing(ThreadID tid) const
    { return doneSquashing[tid]; }

    /** Checks if the FTQ is still in the process of squashing instructions for
     *  any thread.
     */
    bool isDoneSquashing();

    /** Reset the FTQ state */
    void resetState();























  private:


    /** Pointer to the CPU. */
    CPU *cpu;

    /** Active Threads in CPU */
    std::list<ThreadID> *activeThreads;

    /** Number of instructions in the FTQ. */
    unsigned numEntries;

    // /** Entries Per Thread */
    // unsigned threadEntries[MaxThreads];

    /** Max Insts a Thread Can Have in the FTQ */
    unsigned maxEntries[MaxThreads];

    /** FTQ List of Fetch targets */
    std::list<FetchTargetPtr> ftq[MaxThreads];

    /** Number of instructions that can be squashed in a single cycle. */
    unsigned squashWidth;

  public:



  public:
    /** Number of instructions in the FTQ. */
    int numFetchTargetsInFTQ;

  private:
    /** The sequence number of the squashed instruction. */
    InstSeqNum squashedSeqNum[MaxThreads];

    /** Is the FTQ done squashing. */
    bool doneSquashing[MaxThreads];

    /** Number of active threads. */
    ThreadID numThreads;


    struct FTQStats : public statistics::Group
    {
        FTQStats(statistics::Group *parent);

        // The number of ftq_reads
        statistics::Scalar reads;
        // The number of ftq_writes
        statistics::Scalar writes;
    } stats;
};

} // namespace o3
} // namespace gem5

#endif //__CPU_O3_FTQ_HH__
