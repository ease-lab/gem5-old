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

#include "cpu/o3/probe/inst_trace.hh"

#include "base/trace.hh"
#include "cpu/o3/dyn_inst.hh"
#include "debug/InstTrace.hh"

namespace gem5
{

namespace o3
{

void
InstTrace::traceCommit(const DynInstConstPtr& dynInst)
{
    if (trace_commit) {
        DPRINTFR(InstTrace, "[%s]: Commit 0x%08x %s.\n", name(),
             dynInst->pcState().instAddr(),
             dynInst->staticInst->disassemble(dynInst->pcState().instAddr()));
    }

    if (trace_branches && dynInst->isControl()) {
        traceBranch(dynInst);
    }
    if (trace_memref && dynInst->isMemRef()) {
        traceMemRef(dynInst);
    }

}

void
InstTrace::traceFetch(const DynInstConstPtr& dynInst)
{
    if (trace_fetch) {
        DPRINTFR(InstTrace, "[%s]: Fetch 0x%08x %s.\n", name(),
             dynInst->pcState().instAddr(),
             dynInst->staticInst->disassemble(dynInst->pcState().instAddr()));
    }
}

void
InstTrace::regProbeListeners()
{
    typedef ProbeListenerArg<InstTrace,
            DynInstConstPtr> DynInstListener;
    listeners.push_back(new DynInstListener(this, "Commit",
                &InstTrace::traceCommit));
    listeners.push_back(new DynInstListener(this, "Fetch",
                &InstTrace::traceFetch));
}

void
InstTrace::traceBranch(const DynInstConstPtr& dynInst)
{
    // DPRINTFR(InstTrace, "Commit branch: 0x%08x %s.\n",
    //       dynInst->pcState().instAddr(),
    //       dynInst->staticInst->disassemble(dynInst->pcState().instAddr()));

    auto pc_addr = dynInst->pcState().instAddr();
    auto target = dynInst->branchTarget()->instAddr();
    auto conditional = dynInst->isCondCtrl();
    auto direct = dynInst->isDirectCtrl();
    std::string type = "";
    int64_t distance = int64_t(pc_addr - target);
    auto taken = (distance > 4) ? true : false;

    if (dynInst->isCall()) {
        type = "call";
    } else if (dynInst->isReturn()) {
        type = "return";
    } else if (dynInst->isUncondCtrl() || dynInst->isCondCtrl()) {
        type = "jump";
    } else {
        type = "other";
    }

    DPRINTFR(InstTrace, "[%s]: Branch | 0x%08x, "
        "type:%s, cond:%i, direct:%i, taken:%i, dist:%i, target: 0x%08x "
        "| %s.\n", name(),
        pc_addr, type, conditional, direct, taken, distance, target,
        dynInst->staticInst->disassemble(pc_addr));

}

void
InstTrace::traceMemRef(const DynInstConstPtr& dynInst)
{
    // DPRINTFR(InstTrace, "Commit mem: 0x%08x %s.\n",
    //       dynInst->pcState().instAddr(),
    //       dynInst->staticInst->disassemble(dynInst->pcState().instAddr()));

    auto pc_addr = dynInst->pcState().instAddr();
    auto addr = dynInst->effAddr;
    auto paddr = dynInst->physEffAddr;
    std::string type = dynInst->isLoad() ? "ld" : "st";


    DPRINTFR(InstTrace, "[%s]: Mem | 0x%08x %s -> 0x%08x (0x%08x). | %s.\n",
        name(), pc_addr, type, addr, paddr,
        dynInst->staticInst->disassemble(pc_addr));

}


} // namespace o3
} // namespace gem5
