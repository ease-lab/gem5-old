/*
 * Copyright (c) 2021 Huawei Technologies Switzerland
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

#include "cpu/pred/perfect.hh"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <zlib.h>

#include <iterator>
#include <list>

#include "base/logging.hh"
#include "base/named.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/static_inst.hh"
#include "debug/Branch.hh"

namespace gem5
{
namespace branch_prediction
{
PerfectBP::PerfectBP(const PerfectBPParams &params)
    : BPredUnit(params), numberEntriesToFetch(params.numberEntriesToFetch)
{
    panic_if(params.numThreads > 1,
            "PerfectBP option is only working for 1 core with no SMT");
    panic_if(numberEntriesToFetch == 0,
            "numberEntriesToFetch must be greater than 0.");

    // Open trace file
    int fd = open(params.traceFile.c_str(), O_RDONLY);
    panic_if(fd <= 0, "Failed to open trace file '%s'\n",
            params.traceFile.c_str());

    // Open to inflate gzip
    // If the file is not compressed, then it will be read normally
    traceBpFd = gzdopen(fd, "rb");
    panic_if(!traceBpFd, "Failed to gzopen() trace file.\n");

    currBranch = takenBranchesTrace.begin();
    fetchNewEntries();
}

bool
PerfectBP::predict(const StaticInstPtr &inst, const InstSeqNum &seq_num,
        PCStateBase &pc, [[maybe_unused]] ThreadID tid)
{
    if (currBranch == takenBranchesTrace.end()) {
        // If we have read all the entries from the trace list and the trace
        // file does not contain anymore entries:
        // The branch will not be taken
        // Typical case in SE mode: branches past the exit syscall
        if (traceFileDone) {
            DPRINTF(Branch,
                    "[sn:%lu], pc: %llx, Past the end of file."
                    " Predicted not taken\n",
                    seq_num, pc.instAddr());
            return false;
        } else {
            fetchNewEntries();
        }
    }


    // Match the current pc with the first unused branch pc from trace
    // If match, this mean that the branch will be taken
    if (pc.instAddr() == currBranch->pc && pc.microPC() == currBranch->upc) {
        DPRINTF(Branch,
                "[sn:%lu] PC %lx, uPC %d: Predicted taken to"
                " %lx\n",
                seq_num, currBranch->pc, currBranch->upc, currBranch->npc);

        // Set the seqNum for the squash and the release of entries
        currBranch->seqNum = seq_num;

        pc.resetBranchingPCFromArgs(currBranch->pc, currBranch->npc,
                currBranch->upc, currBranch->nupc);

        // Inc the offset in the trace list
        currBranch++;
        return true;
    } else {
        // Branch not taken, update pc
        DPRINTF(Branch,
                "[sn:%lu] PC: %lx, next taken PC: %lx Predicted not taken\n",
                seq_num, pc.instAddr(), currBranch->pc);
        inst->advancePC(pc);
        return false;
    }
}

void
PerfectBP::squash(const InstSeqNum &squashedSn, [[maybe_unused]] ThreadID tid)
{
    // We rewind the iterator before the squashed entries
    // No entries are removed from the list
    // Squashed entries have their seqNum cleared

    // currBranch point to the first entry not yet used
    while (currBranch != takenBranchesTrace.begin() &&
            std::prev(currBranch)->seqNum > squashedSn) {
        currBranch--;
        // Clear trace seqNum
        currBranch->seqNum = 0;
    }
}

void
PerfectBP::update(const InstSeqNum &seq_num, [[maybe_unused]] ThreadID tid)
{
    // Remove committed entries from the list
    while (takenBranchesTrace.begin() != currBranch &&
            takenBranchesTrace.begin()->seqNum <= seq_num) {
        takenBranchesTrace.pop_front();
    }
}

void
PerfectBP::fetchNewEntries()
{
    // At the start of the function currBranch point to the end of the list
    // It will be invalidated on list.push_back()
    // Save the distance to the begin and restore it after the allocations
    int distance = std::distance(takenBranchesTrace.begin(), currBranch);

    for (size_t num_fetched = 0; num_fetched < numberEntriesToFetch;
            ++num_fetched) {
        if (!fetchOneEntry()) {
            traceFileDone = true;
            break;
        }
    }

    currBranch = std::next(takenBranchesTrace.begin(), distance);
}

bool
PerfectBP::fetchOneEntry()
{
    // Trace format is 64bit pc, 16 bit upc and 64bit npc.
    uint64_t pc, npc;
    uint16_t upc, nupc;
    int read = 0;

    // Read pc
    read = gzread(traceBpFd, (char *)&pc, 8);
    // Test if the file is fully read
    if (read == 0)
        return false;

    // Read upc
    read = gzread(traceBpFd, (char *)&upc, 2);
    panic_if(read != 2, "BP trace file corrupted");

    // Read npc
    read = gzread(traceBpFd, (char *)&npc, 8);
    panic_if(read != 8, "BP trace file corrupted");

    // Read nupc
    read = gzread(traceBpFd, (char *)&nupc, 2);
    panic_if(read != 2, "BP trace file corrupted");

    struct BPTraceEntry entry = BPTraceEntry{pc, upc, npc, nupc, 0};
    takenBranchesTrace.push_back(entry);

    // Return false if we are at the end of the file
    return !gzeof(traceBpFd);
}

} // namespace branch_prediction
} // namespace gem5
