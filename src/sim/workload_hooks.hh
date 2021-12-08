/*
 * Copyright (c) 2021 EASE Group, The University of Edinburgh
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


#ifndef __ARCH_X86_LINUX_WL_HOCKS_HH__
#define __ARCH_X86_LINUX_WL_HOCKS_HH__

#include <tuple>

#include "base/output.hh"

// #include "kern/linux/events.hh"
// #include "base/loader/symtab.hh"
#include "cpu/pc_event.hh"
#include "params/WorkloadHooks.hh"
#include "sim/sim_object.hh"
#include "sim/stats.hh"

// #include "base/loader/object_file.hh"
// #include "base/loader/symtab.hh"
// #include "base/types.hh"
// #include "params/KernelWorkload.hh"
#include "sim/probe/probe.hh"
#include "sim/workload.hh"

namespace gem5
{

class FunctionHook : public PCEvent
{
  public:
    FunctionHook(PCEventScope *s, const std::string &desc, Addr addr,
                    WorkloadHooks *_parent)
        : PCEvent(s, desc, addr), parent(_parent)
    {}
    WorkloadHooks *parent;
    virtual void regProbePoints(ProbeManager *pm) {};
};

class WorkloadHooks : public SimObject
{
  protected:

  /** The Workload where you want to hock up function events to */
    Workload *workload;

    FunctionHook *switchToEvent = nullptr;
    FunctionHook *getGidEvent = nullptr;
    // /** When enabled, dump stats/task info on context switches for
    //  *  Streamline and per-thread cache occupancy studies, etc. */
    const bool enableContextSwitchHook;
    const bool enableGetGIDHook;


  public:
    typedef std::tuple<int, int, int> Filter;
    std::vector<Filter> filter;

    const bool exitOnCS;

  public:
    typedef WorkloadHooksParams Params;
    WorkloadHooks(const Params &p);


    // void init() override
    void init() override {
      hookUp();
      // switchToEvent->regProbePoints(this->getProbeManager());
    }
    void regProbePoints() override {
      switchToEvent->regProbePoints(this->getProbeManager());
    }

    bool hookUp();


    /** This map stores a mapping of OS process IDs to internal Task IDs. The
     * mapping is done because the stats system doesn't tend to like vectors
     * that are much greater than 1000 items and the entire process space is
     * 65K. */
    std::map<uint32_t, uint32_t> taskMap;

    /** This is a file that is placed in the run directory that prints out
     * mappings between taskIds and OS process IDs */
    OutputStream *taskFile = nullptr;

    /** This function creates a new task Id for the given pid.
     * @param tc thread context that is currentyl executing  */
    void mapPid(ThreadContext* tc, uint32_t pid);

        /** @{ */
    // /**
    //  * Add a function-based event to this a kernel symbol.
    //  *
    //  * These functions work like their addFuncEvent() and
    //  * addFuncEventOrPanic() counterparts. The only difference is that
    //  * they automatically use the kernel symbol table. All arguments
    //  * are forwarded to the underlying method.
    //  *
    //  * @see addFuncEvent()
    //  * @see addFuncEventOrPanic()
    //  *
    //  * @param lbl Function to hook the event to.
    //  * @param args Arguments to be passed to addFuncEvent
    //  */
    // template <class T, typename... Args>
    // T *
    // addKernelFuncEvent(const char *lbl, Args... args)
    // {
    //     return addFuncEvent<T>(kernelSymtab, lbl,
    //        std::forward<Args>(args)...);
    // }

    // template <class T, typename... Args>
    // T *
    // addKernelFuncEventOrPanic(const char *lbl, Args... args)
    // {
    //     T *e = addFuncEvent<T>(kernelSymtab, lbl,
    //std::forward<Args>(args)...);
    //     panic_if(!e, "Failed to find kernel symbol '%s'", lbl);
    //     return e;
    // }


    /**
     * Function to add a context switch filter.
     *
     * By default the hook will trigger exits and prob notifies on every
     * single context switch. With this function one can specify to trigger
     * only on certain events. You can filter by cpu, next, and previous
     * PID. If only one parameter matter set the others to a negative value.
     * You can add more than one filter
     *
     * @param cpu_id The cpu where the CS should happen
     * @param prev_pid Only tigger if the switch is from a a certain PID.
     * @param next_pid Only tigger if the switch is to a a certain PID.
     */
    void addCSFilterConfig(int cpu_id =-1,int prev_pid =-1, int next_pid =-1) {
      filter.push_back(std::make_tuple(cpu_id,prev_pid,next_pid));
    }
    void clearCSFilter() {
      filter.clear();
    }

};





class DumpStats : public PCEvent
{
  public:
    DumpStats(PCEventScope *s, const std::string &desc, Addr addr)
        : PCEvent(s, desc, addr)
    {}

    void process(ThreadContext* tc) override;
  protected:
    virtual void getTaskDetails(ThreadContext *tc, uint32_t &pid,
            uint32_t &tgid, std::string &next_task_str, int32_t &mm);

};

class DumpStats64 : public DumpStats
{
  public:
    using DumpStats::DumpStats;

  private:
    void getTaskDetails(ThreadContext *tc, uint32_t &pid, uint32_t &tgid,
                        std::string &next_task_str, int32_t &mm) override;
};


class SwitchToEvent : public FunctionHook
{
  public:
    SwitchToEvent(PCEventScope *s, const std::string &desc,
                    Addr addr, WorkloadHooks *_parent)
        : FunctionHook(s, desc, addr, _parent)
    {}

    void process(ThreadContext* tc) override;
    void regProbePoints(ProbeManager *pm) override {
      ppSwitchIdleActive = new ProbePointArg<bool>(pm, "SwitchActiveIdle");
    }

  protected:
    // void getTaskDetails(ThreadContext *tc, uint32_t &pid,
    //         uint32_t &tgid, std::string &next_task_str, int32_t &mm);

    ProbePoint *ppSwitch;
    /** To probe when one of the corese switches between idle and active.
     * The probe will call the all listeners with true if its a switch to
     * active false if switch to idle;
     */
    ProbePointArg<bool> *ppSwitchIdleActive;
};

class GetGIDEvent : public FunctionHook
{
  public:
    GetGIDEvent(PCEventScope *s, const std::string &desc,
                  Addr addr, WorkloadHooks *_parent)
        : FunctionHook(s, desc, addr, _parent)
    {}

    void process(ThreadContext* tc) override;
  protected:
    // void getTaskDetails(ThreadContext *tc, uint32_t &pid,
    //         uint32_t &tgid, std::string &next_task_str, int32_t &mm);

};

} // namespace gem5

#endif // __ARCH_X86_LINUX_WL_HOCKS_HH__
