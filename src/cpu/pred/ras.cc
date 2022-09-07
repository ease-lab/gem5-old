/*
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

#include "cpu/pred/ras.hh"

#include <iomanip>

#include "debug/RAS.hh"

namespace gem5
{

namespace branch_prediction
{



void
ReturnAddrStack::AddrStack::init(unsigned _numEntries)
{
    numEntries = _numEntries;
    addrStack.resize(numEntries);
    for (unsigned i = 0; i < numEntries; ++i) {
        addrStack[i] = nullptr;
    }
    reset();
}

void
ReturnAddrStack::AddrStack::reset()
{
    usedEntries = 0;
    tos = 0;
}

const PCStateBase *
ReturnAddrStack::AddrStack::top()
{
    if (usedEntries > 0) {
        return addrStack[tos].get();
    }
    return nullptr;
}


void
ReturnAddrStack::AddrStack::push(const PCStateBase &return_addr)
{

    incrTos();

    set(addrStack[tos], return_addr);

    // if (usedEntries != numEntries) {
        ++usedEntries;
    // }
}

void
ReturnAddrStack::AddrStack::pop()
{
    if (usedEntries > 0) {
        --usedEntries;
        decrTos();
    }
}

void
ReturnAddrStack::AddrStack::restore(unsigned top_entry_idx,
                const PCStateBase *restored)
{
    if (usedEntries != numEntries) {
        ++usedEntries;
    }
    tos = top_entry_idx;
    set(addrStack[tos], restored);
}


std::string
ReturnAddrStack::AddrStack::print(int n)
{
    std::stringstream ss;
    ss << "";
    for (int i = 0; i<n; i++) {
        int idx = int(tos)-i;
        if (idx < 0 || addrStack[idx] == nullptr) {
            break;
        }
        ss << std::dec << idx << ":0x" << std::setfill('0') << std::setw(8)
           << std::hex << addrStack[idx]->instAddr() << ";";
    }
    return ss.str();
}



ReturnAddrStack::ReturnAddrStack(const Params &p)
    : SimObject(p),
      numEntries(p.numEntries),
      numThreads(p.numThreads)
{
    DPRINTF(RAS, "Create RAS stacks.\n");

    for (unsigned i = 0; i < numThreads; ++i) {
        addrStacks.emplace_back(*this);
        addrStacks[i].init(numEntries);
    }
}



void
ReturnAddrStack::reset()
{
    DPRINTF(RAS, "RAS Reset.\n");
    for (auto& r : addrStacks)
        r.reset();
}

void
ReturnAddrStack::push(const PCStateBase &return_addr, ThreadID tid)
{
    addrStacks[tid].push(return_addr);

    // DPRINTF(Branch, "RAS Push: RAS[%i] <= %llu. Entries used: %i\n",
    //                 tos,return_addr,  usedEntries);
    DPRINTF(RAS, "Push:  RAS[%i] <= 0x%08x. Entries used: %i, tid:%i\n",
                    addrStacks[tid].tos,
                    return_addr.instAddr(),
                    addrStacks[tid].usedEntries,tid);
    DPRINTF(RAS, "[%s]\n", addrStacks[tid].print(10));
}

void
ReturnAddrStack::pop(ThreadID tid)
{
    addrStacks[tid].pop();

    DPRINTF(RAS, "Pop:   RAS[%i] => x. "
                "Entries used: %i, tid:%i\n",
                addrStacks[tid].tos, addrStacks[tid].usedEntries, tid);
    DPRINTF(RAS, "[%s]\n", addrStacks[tid].print(10));
}

void
ReturnAddrStack::restore(unsigned top_entry_idx,
                         const PCStateBase *restored,
                         ThreadID tid)
{
    addrStacks[tid].restore(top_entry_idx, restored);
    // if (usedEntries != numEntries) {
    //     ++usedEntries;
    // }
    DPRINTF(RAS, "Rest.: RAS[%i] => RAS[%i]:0x%08x. "
            "Entries used: %i, tid:%i\n",
            addrStacks[tid].tos,
            top_entry_idx, restored->instAddr(),
            addrStacks[tid].usedEntries, tid);
    DPRINTF(RAS, "[%s]\n", addrStacks[tid].print(10));
}

} // namespace branch_prediction
} // namespace gem5
