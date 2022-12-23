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

#include "base/cprintf.hh"
#include "base/loader/symtab.hh"
#include "base/output.hh"
#include "base/trace.hh"
#include "cpu/o3/dyn_inst.hh"
#include "debug/InstTrace.hh"

namespace gem5
{

namespace o3
{

InstTrace::InstTrace(const InstTraceParams &params)
    : ProbeListenerObject(params),
      trace_fetch(params.trace_fetch),
      trace_commit(params.trace_commit),
      trace_branches(params.trace_branches),
      trace_memref(params.trace_memref),
      trace_func(params.trace_functions),
      call_stack_idx(0),
      traceStream(nullptr),
      functionTraceStream(nullptr)
{
    std::string filename = simout.resolve(params.instTraceFile);
    traceStream = new ProtoOutputStream(filename);

    ProtoMessage::InstTraceHeader inst_pkt_header;
    inst_pkt_header.set_obj_id(name());
    inst_pkt_header.set_tick_freq(sim_clock::Frequency);
    inst_pkt_header.set_has_mem(trace_memref);
    inst_pkt_header.set_has_fetch(trace_fetch);
    traceStream->write(inst_pkt_header);

    const std::string fname = csprintf("ftrace.%s", name());
    functionTraceStream = simout.findOrCreate(fname)->stream();

    // Register a callback to flush trace records and close the output stream.
    registerExitCallback([this]() {  flushTrace(); });
}

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
    if (trace_func && dynInst->isControl()) {
        traceFunction(dynInst);
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

    uint64_t pc_addr = dynInst->pcState().instAddr();
    uint64_t target = dynInst->branchTarget()->instAddr();
    BranchClass type = getBranchClass(dynInst->staticInst);
    // std::string type = "";
    int64_t distance = int64_t(pc_addr - target);
    bool pred_taken = dynInst->readPredTaken();
    // bool mispred = dynInst->mispredicted();
    bool actually_taken = dynInst->pcState().branching();

    DPRINTFR(InstTrace, "[%s]: Branch | 0x%08x, "
        "%s, pred:%i,mispred:%i, dist:%i, target: 0x%08x "
        "| %s.\n", name(),
        pc_addr, toStr(type),
        pred_taken, pred_taken != actually_taken,
        distance, target,
        dynInst->staticInst->disassemble(pc_addr));

    ProtoMessage::InstRecord pkt;
    pkt.set_pc(pc_addr);
    pkt.set_tick(curTick());
    pkt.set_type(Inst::Control);
    pkt.set_brtype((BranchType)type);
    pkt.set_target(target);
    pkt.set_taken(actually_taken);
    pkt.set_pred_taken(pred_taken);
    pkt.set_resteered(dynInst->readResteered());

    pkt.set_fe_tick(dynInst->fetchTick);
    pkt.set_de_tick(dynInst->decodeTick);
    pkt.set_is_tick(dynInst->issueTick);
    pkt.set_co_tick(dynInst->commitTick);

    // Write the message to the protobuf output stream
    traceStream->write(pkt);
}

void
InstTrace::traceMemRef(const DynInstConstPtr& dynInst)
{
    // DPRINTFR(InstTrace, "Commit mem: 0x%08x %s.\n",
    //       dynInst->pcState().instAddr(),
    //       dynInst->staticInst->disassemble(dynInst->pcState().instAddr()));

    uint64_t pc_addr = dynInst->pcState().instAddr();
    uint64_t addr = dynInst->effAddr;
    uint64_t paddr = dynInst->physEffAddr;
    std::string type = dynInst->isLoad() ? "ld" : "st";
    // assert(dyn)
    uint64_t val = (dynInst->traceData) ?
                            dynInst->traceData->getIntData() : MaxAddr;
    MemType mtype = dynInst->isLoad() ? Inst::MemRead : Inst::MemWrite;

    bool use_stack = false;
    for (int i = 0; i < dynInst->numSrcRegs(); i++) {
        if (dynInst->srcRegIdx(i) == X86ISA::int_reg::Rsp) {
            use_stack = true;
            break;
        }
    }



    DPRINTFR(InstTrace, "[%s]: Mem | 0x%08x %s -> 0x%08x (0x%08x). "
        "val: %#x| %s.\n", name(), pc_addr, type, addr, paddr, val,
        dynInst->staticInst->disassemble(pc_addr));

    ProtoMessage::InstRecord pkt;
    pkt.set_pc(pc_addr);
    pkt.set_tick(curTick());
    pkt.set_type(Inst::Mem);
    pkt.set_mtype(mtype);
    pkt.set_p_addr(paddr);
    pkt.set_v_addr(addr);
    pkt.set_value(val);
    pkt.set_use_stack(use_stack);

    pkt.set_fe_tick(dynInst->fetchTick);
    pkt.set_de_tick(dynInst->decodeTick);
    pkt.set_is_tick(dynInst->issueTick);
    pkt.set_co_tick(dynInst->commitTick);


    // Write the message to the protobuf output stream
    traceStream->write(pkt);

}


enums::BranchClass
InstTrace::getBranchClass(StaticInstPtr inst)
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
InstTrace::traceFunction(const DynInstConstPtr& dynInst)
{
    if (loader::debugSymbolTable.empty())
        return;

    uint64_t pc_addr = dynInst->pcState().instAddr();
    uint64_t target = dynInst->branchTarget()->instAddr();
    BranchClass type = getBranchClass(dynInst->staticInst);

    // DPRINTFR(InstTrace, "[%s]: Branch | 0x%08x, "
    //     "%s, pred:%i,mispred:%i, dist:%i, target: 0x%08x "
    //     "| %s.\n", name(),
    //     pc_addr, toStr(type),
    //     pred_taken, pred_taken != actually_taken,
    //     distance, target,
    //     dynInst->staticInst->disassemble(pc_addr));

    if ((type != BranchClass::CallDirect) && (type != BranchClass::Return)) {
        return;
    }

    auto it = loader::debugSymbolTable.findNearest(target);

    std::string sym_str;
    if (it == loader::debugSymbolTable.end()) {
        // no symbol found: use addr as label
        sym_str = csprintf("%#x", target);
    } else {
        sym_str = it->name;
    }

    bool func_enter = false;
    // Check if we enter a function
    if (type == BranchClass::CallDirect) {
        call_stack_idx++;
        func_enter = true;
    } else {
        call_stack_idx--;
    }


    ccprintf(*functionTraceStream, "%d: CSP[%d]: "
                                   "%#x(%#x) %s %#x(%#x) : %s\n",
                 curTick(), call_stack_idx,
                 pc_addr, addrBlockAlign(pc_addr,64),
                 func_enter ? "call ->" : "ret ->",
                 target, addrBlockAlign(target,64),
                 sym_str);


}

void
InstTrace::flushTrace()
{
    // // Write to trace all records in the depTrace.
    // writeDepTrace(depTrace.size());
    // Delete the stream objects
    delete traceStream;
    delete functionTraceStream;
}


} // namespace o3
} // namespace gem5
