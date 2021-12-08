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

#include <cstdio>
#include <string>

// #include <vector>
#include "sim/workload_hooks.hh"

#include <map>

#include "arch/generic/linux/threadinfo.hh"

// #include "base/loader/dtb_file.hh"
// #include "base/loader/object_file.hh"
#include "base/loader/symtab.hh"
#include "cpu/base.hh"

// #include "cpu/pc_event.hh"
// #include "cpu/thread_context.hh"
#include "debug/Loader.hh"

// #include "kern/linux/events.hh"
// #include "kern/linux/helpers.hh"
// #include "kern/system_events.hh"

#include "arch/x86/regs/int.hh"

// #include "base/trace.hh"
#include "cpu/thread_context.hh"

// #include "mem/port_proxy.hh"
// #include "params/X86FsLinux.hh"
// #include "sim/byteswap.hh"
// #include "sim/system.hh"
#include "sim/sim_exit.hh"
#include "sim/workload.hh"

// #include "sim/stat_control.hh"



namespace gem5
{

// using namespace linux;


WorkloadHooks::WorkloadHooks(const Params &p) :
    SimObject(p),
    workload(p.workload),
    enableContextSwitchHook(p.enable_context_switch_hook),
    enableGetGIDHook(p.enable_getgid_hook),
    exitOnCS(p.exit_on_context_switch)
{}

bool
WorkloadHooks::hookUp()
{
    warn("HookUp called");
    if (enableContextSwitchHook) {
        panic_if(workload->getArch() != loader::X86_64,
                                "switchToEvent not created!");

        switchToEvent = workload->addFuncEvent<SwitchToEvent>(
                                workload->symtab(nullptr),
                                "__switch_to","__switch_to", this);

        panic_if(!switchToEvent, "switchToEvent not created!");

        // switchToEvent->parent = this;

        std::string task_filename = "tasks.txt";
        taskFile = simout.create(name() + "." + task_filename);

        // for (auto *tc: system->threads) {
        //     uint32_t pid = tc->getCpuPtr()->getPid();
        //     if (pid != BaseCPU::invldPid) {
        //         mapPid(tc, pid);
        //         tc->getCpuPtr()->taskId(taskMap[pid]);
        //     }
        // }
    }

    if (enableGetGIDHook) {
        getGidEvent = workload->addFuncEvent<GetGIDEvent>(
                            workload->symtab(nullptr),
                            "__x64_sys_getgid","__x64_sys_getgid", this);
        warn("Hock up to '__x64_sys_getgid'");
        panic_if(!getGidEvent, "getGidEvent not created!");
        // getGidEvent->parent = this;
    }
    return true;

}

void
WorkloadHooks::mapPid(ThreadContext *tc, uint32_t pid)
{
    // Create a new unique identifier for this pid
    std::map<uint32_t, uint32_t>::iterator itr = taskMap.find(pid);
    if (itr == taskMap.end()) {
        uint32_t map_size = taskMap.size();
        if (map_size > context_switch_task_id::MaxNormalTaskId + 1) {
            warn_once("Error out of identifiers for cache occupancy stats");
            taskMap[pid] = context_switch_task_id::Unknown;
        } else {
            taskMap[pid] = map_size;
        }
    }
}



// /**
//  * Extracts the information used by the DumpStatsPCEvent by reading the
//  * thread_info pointer passed to __switch_to() in 32 bit ARM Linux
//  *
//  *  r0 = task_struct of the previously running process
//  *  r1 = thread_info of the previously running process
//  *  r2 = thread_info of the next process to run
//  */
// void
// DumpStats::getTaskDetails(ThreadContext *tc, uint32_t &pid,
//     uint32_t &tgid, std::string &next_task_str, int32_t &mm) {

//     linux::ThreadInfo ti(tc);
//     Addr task_descriptor = tc->readIntReg(2);
//     pid = ti.curTaskPID(task_descriptor);
//     tgid = ti.curTaskTGID(task_descriptor);
//     next_task_str = ti.curTaskName(task_descriptor);

//     // Streamline treats pid == -1 as the kernel process.
//     // Also pid == 0 implies idle process (except during Linux boot)
//     mm = ti.curTaskMm(task_descriptor);
// }

// /**
//  * Extracts the information used by the DumpStatsPCEvent64 by reading the
//  * task_struct pointer passed to __switch_to() in 64 bit x86 Linux
//  *
//  *  %rdi = 1st arg = task_struct of the previously running process
//  *  %rsi = 2nd arg = task_struct of next process to run
//  */
// void
// DumpStats64::getTaskDetails(ThreadContext *tc, uint32_t &pid,
//     uint32_t &tgid, std::string &next_task_str, int32_t &mm) {

//     // linux::ThreadInfo ti(tc);
//     warn("__switch_to: rdi[%i], rsi[%i],",
//         tc->readIntReg(INTREG_RDI), tc->readIntReg(INTREG_RSI));
//     // Addr task_struct = tc->readIntReg(INTREG_RSI);
//     // Process *p = tc->getProcessPtr();




//     // if (p != nullptr) {
//     //         pid = p->pid();
//     // tgid = p->tgid();
//     // }

//     linux::ThreadInfo ti(tc);
//     Addr task_struct = tc->readIntReg(INTREG_RSI);
//     pid = ti.curTaskPIDFromTaskStruct(task_struct);
//     tgid = ti.curTaskTGIDFromTaskStruct(task_struct);
//     next_task_str = ti.curTaskNameFromTaskStruct(task_struct);

//     // Streamline treats pid == -1 as the kernel process.
//     // Also pid == 0 implies idle process (except during Linux boot)
//     mm = ti.curTaskMmFromTaskStruct(task_struct);

//   warn("__switch_to: tgid: %i, cpuID %i, socketId %i, contextID %i, pid %i",
//         tgid, tc->cpuId(),
//         tc->socketId(), tc->contextId(), pid);
// }

// /** This function is called whenever the the kernel function
//  *  "__switch_to" is called to change running tasks.
//  */
// void
// DumpStats::process(ThreadContext *tc)
// {
//     uint32_t pid = 0;
//     uint32_t tgid = 0;
//     std::string next_task_str;
//     int32_t mm = 0;

//     // warn("Want to get task_info");
//     getTaskDetails(tc, pid, tgid, next_task_str, mm);

//     bool is_kernel = (mm == 0);
//     if (is_kernel && (pid != 0)) {
//         pid = -1;
//         tgid = -1;
//         next_task_str = "kernel";
//     }

//     // FsLinux* wl = dynamic_cast<FsLinux *>(tc->getSystemPtr()->workload);
//     panic_if(!wl, "System workload is not ARM Linux!");
//     std::map<uint32_t, uint32_t>& taskMap = wl->taskMap;

//     // Create a new unique identifier for this pid
//     wl->mapPid(tc, pid);

//     // Set cpu task id, output process info, and dump stats
//     tc->getCpuPtr()->taskId(taskMap[pid]);
//     tc->getCpuPtr()->setPid(pid);

//     OutputStream* taskFile = wl->taskFile;

//     // Task file is read by cache occupancy plotting script or
//     // Streamline conversion script.
//     ccprintf(*(taskFile->stream()),
//            "tick=%lld %d cpu_id=%d next_pid=%d next_tgid=%d next_task=%s\n",
//              curTick(), taskMap[pid], tc->cpuId(), (int)pid, (int)tgid,
//              next_task_str);
//     taskFile->stream()->flush();

//     // Dump and reset statistics
    // statistics::schedStatEvent(true, true, curTick(), 0);
// }






/** This function is called whenever the the kernel function
 *  "__switch_to" is called to change running tasks.
 */
void
SwitchToEvent::process(ThreadContext *tc)
{
    // warn("Want to get task_info");
    // linux::ThreadInfo ti(tc);
    // warn("__switch_to(prev[rdi[%i]], next[rsi[%i]]), cpuID %i,
    //socketId %i, contextID %i",
    //     tc->readIntReg(X86ISA::INTREG_RDI),
    //tc->readIntReg(X86ISA::INTREG_RSI),
    //     tc->cpuId(), tc->socketId(), tc->contextId());

    linux::ThreadInfo ti(tc);

    // Prev thread info -------
    Addr task_struct = tc->readIntReg(X86ISA::INTREG_RDI);
    uint32_t prev_pid = ti.curTaskPIDFromTaskStruct(task_struct);
    uint32_t prev_tgid = ti.curTaskTGIDFromTaskStruct(task_struct);
    std::string prev_task_str = ti.curTaskNameFromTaskStruct(task_struct);

    // Next thread info -------
    task_struct = tc->readIntReg(X86ISA::INTREG_RSI);
    uint32_t next_pid = ti.curTaskPIDFromTaskStruct(task_struct);
    uint32_t next_tgid = ti.curTaskTGIDFromTaskStruct(task_struct);
    std::string next_task_str = ti.curTaskNameFromTaskStruct(task_struct);

    // Also pid == 0 implies idle process (except during Linux boot)
    int32_t mm = ti.curTaskMmFromTaskStruct(task_struct);

    // bool is_kernel = (mm == 0);
    // if (is_kernel && (next_pid != 0)) {
    //     next_pid = -1;
    //     next_tgid = -1;
    //     next_task_str = "kernel";
    // }

    // Create a new unique identifier for this pid
    parent->mapPid(tc, next_pid);

    auto str = csprintf("tick=%lld cpu_id=%d "
                        "__switch_to(prev [%i/%i], next [%i/%i]) %s -> %s\n",
             curTick(), tc->cpuId(),
             (int)prev_pid, (int)prev_tgid, (int)next_pid, (int)next_tgid,
             prev_task_str, next_task_str);
    // warn(str);

    // Task file is read by cache occupancy plotting script or
    // Streamline conversion script.
    ccprintf(*(parent->taskFile->stream()),str);
    parent->taskFile->stream()->flush();

    // In the following we will check if this is a context switch we care about

    bool trigger = false;
    if (parent->filter.size() > 0) {
        for (auto flt : parent->filter) {
            int _cpu_id, _prev_pid, _next_pid;
            std::tie(_cpu_id, _prev_pid, _next_pid) = flt;
            // warn("%s,%s,%s", _cpu_id, _prev_pid, _next_pid);

            // first check if the cpu_id matches
            // If not we can skip the rest and try the next filter
            if (_cpu_id >= 0 && _cpu_id != tc->cpuId()) {
                continue;
            }
            // second check if the previous pid matches
            if (_prev_pid >= 0 && _prev_pid != prev_pid) {
                continue;
            }
            // finally check if the next pid matches
            if (_next_pid >= 0 && _next_pid != next_pid) {
                continue;
            }
            // At this point the filter matches
            // and we can break out the loop.
            trigger = true;
            break;
        }
    } else {
        trigger = true;
    }

    if (!trigger) {
        return;
    }


    // Notify if this was a task switch from or to the Idle task (PID = 0)
    if (prev_pid == 0) {
        ppSwitchIdleActive->notify(true);
    }
    if (next_pid == 0) {
        ppSwitchIdleActive->notify(false);
    }

    if (parent->exitOnCS) {
        // At the moment we send the packet from the drive system we want th
        exitSimLoop(str,0, curTick(),0);
    }
    // warn(str);
    // Dump and reset statistics
    // statistics::schedStatEvent(true, true, curTick(), 0);
}


/** This function is called whenever the the kernel function
 *  "__switch_to" is called to change running tasks.
 */
void
GetGIDEvent::process(ThreadContext *tc)
{
    // warn("Want to get task_info");
    // linux::ThreadInfo ti(tc);
    warn("syscall_get_gid(rdi[%i], rsi[%i], rax[%i]), "
         "cpuID %i, socketId %i, contextID %i",
        tc->readIntReg(X86ISA::INTREG_RDI),
        tc->readIntReg(X86ISA::INTREG_RSI),
        tc->readIntReg(X86ISA::INTREG_RAX),
        tc->cpuId(), tc->socketId(), tc->contextId());

    // FsLinux* wl = dynamic_cast<FsLinux *>(tc->getSystemPtr()->workload);
    OutputStream* taskFile = parent->taskFile;
    ccprintf(*(taskFile->stream()),
        "tick=%lld syscall_get_gid()\n", curTick());
    taskFile->stream()->flush();

    // linux::ThreadInfo ti(tc);

    // // thread info -------
    // Addr task_struct = tc->readIntReg(INTREG_RDI);
    // uint32_t pid = ti.curTaskPIDFromTaskStruct(task_struct);
    // uint32_t tgid = ti.curTaskTGIDFromTaskStruct(task_struct);
    // std::string task_str = ti.curTaskNameFromTaskStruct(task_struct);

    // // Streamline treats pid == -1 as the kernel process.
    // // Also pid == 0 implies idle process (except during Linux boot)
    // int32_t mm = ti.curTaskMmFromTaskStruct(task_struct);

    // bool is_kernel = (mm == 0);
    // // if (is_kernel && (next_pid != 0)) {
    // //     pid = -1;
    // //     tgid = -1;
    // //     next_task_str = "kernel";
    // // }

    // warn("__getgid_to(tgid: %i, pid %i)", tgid, pid);

}

} // namespace gem5
