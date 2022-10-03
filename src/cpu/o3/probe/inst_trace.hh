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

/**
 * @file This file initializes a simple trace unit which listens to
 * a probe point in the fetch and commit stage of the O3 pipeline
 * and simply outputs those events as a dissassembled instruction stream
 * to the trace output.
 */
#ifndef __CPU_O3_PROBE_INST_TRACE_HH__
#define __CPU_O3_PROBE_INST_TRACE_HH__

#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/static_inst.hh"
#include "enums/BranchClass.hh"
#include "params/InstTrace.hh"
#include "proto/inst_trace.pb.h"
#include "proto/protoio.hh"
#include "sim/probe/probe.hh"

namespace gem5
{

namespace o3
{

class InstTrace : public ProbeListenerObject
{

  public:

    /** Trace record types corresponding to instruction node types */
    typedef ProtoMessage::InstRecord::InstType InstType;
    typedef ProtoMessage::InstRecord::MemType MemType;
    typedef ProtoMessage::InstRecord::BranchType BranchType;
    typedef ProtoMessage::InstRecord Inst;
    typedef enums::BranchClass BranchClass;

    InstTrace(const InstTraceParams &params);

    /** Register the probe listeners. */
    void regProbeListeners() override;

    std::string
    name() const override
    {
        return ProbeListenerObject::name() + ".trace";
    }

  private:

    // Enable tracing of fetched instructions.
    bool trace_fetch = false;
    // Enable tracing of all commited instructions.
    bool trace_commit = false;
    // Enable tracing of commited branches instructions.
    bool trace_branches = false;
    // Enable tracing of commited memory reference instructions.
    bool trace_memref = false;


    /** One output stream for the entire simulation.
     * We encode the CPU & system ID so all we need is a single file
     */
    ProtoOutputStream *traceStream;



    void traceFetch(const DynInstConstPtr& dynInst);
    void traceCommit(const DynInstConstPtr& dynInst);

    void traceBranch(const DynInstConstPtr& dynInst);
    void traceMemRef(const DynInstConstPtr& dynInst);

    BranchClass getBranchClass(StaticInstPtr inst);

    std::string toStr(BranchClass type) const
    {
        return std::string(enums::BranchClassStrings[type]);
    }

    void flushTrace();

};

} // namespace o3
} // namespace gem5

#endif//__CPU_O3_PROBE_INST_TRACE_HH__
