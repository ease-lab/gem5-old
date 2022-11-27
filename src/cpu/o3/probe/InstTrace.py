# Copyright (c) 2022 The University of Edinburgh
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from m5.objects.Probe import *

class InstTrace(ProbeListenerObject):
    type = 'InstTrace'
    cxx_class = 'gem5::o3::InstTrace'
    cxx_header = 'cpu/o3/probe/inst_trace.hh'

    # Trace file for the following params are created in the output directory.
    instTraceFile = Param.String("inst_trace.gz", "Protobuf trace file name")
    funcTraceFile = Param.String("func_trace.txt", "Protobuf trace file name")

# # The committed instruction count from which to start tracing
# startTraceInst = Param.UInt64(0, "The number of committed instructions " \
#                                 "after which to start tracing. Default " \
#                                 "zero means start tracing from first " \
#                                 "committed instruction.")
# # Whether to trace virtual addresses for memory accesses
# traceVirtAddr = Param.Bool(False, "Set to true if virtual addresses are " \
#                             "to be traced.")
    # Whether to trace fetch
    trace_fetch = Param.Bool(False, "Set to true if fetch are to be "
                                    "traced as well.")

    # Whether to trace all commited instructions
    trace_commit = Param.Bool(False, "Trace all commited instructions.")
    trace_branches = Param.Bool(False, "Trace branches")
    trace_memref = Param.Bool(False, "Trace memory references")
    trace_functions = Param.Bool(False, "Trace memory references")
