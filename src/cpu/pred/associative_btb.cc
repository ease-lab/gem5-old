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

#include "cpu/pred/associative_btb.hh"

#include "base/intmath.hh"
#include "base/trace.hh"
#include "debug/BTB.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"

// #include "mem/cache/replacement_policies/base.hh"

namespace gem5
{

namespace branch_prediction
{

AssociativeBTB::AssociativeBTB(const AssociativeBTBParams &p)
    : BranchTargetBuffer(p),
        btb(p.assoc, p.numEntries, p.indexing_policy,
            p.replacement_policy),
        numEntries(p.numEntries),
        assoc(p.assoc),
        tagBits(p.tagBits),
        instShiftAmt(p.instShiftAmt)
{
    if (!isPowerOf2(numEntries)) {
        fatal("BTB entries is not a power of 2!");
    }
    if (!isPowerOf2(assoc)) {
        fatal("BTB associativity is not a power of 2!");
    }

    // The number of entries is divided into n ways.
    uint64_t setBits = floorLog2(numEntries/assoc);

    idxBits = tagBits + setBits;
    idxMask = (idxBits < 64) ? (1ULL << idxBits) - 1 : (uint64_t)(-1);

    DPRINTF(BTB, "BTB: Creating BTB object. entries:%i, assoc:%i, "
                 "tagBits:%i, idx mask:%x\n",
                 numEntries, assoc, tagBits, idxMask);
}

void
AssociativeBTB::reset()
{
    DPRINTF(BTB, "BTB: Invalidate all entries\n");

    for (auto &entry : btb) {
        entry.invalidate();
    }
}

inline
uint64_t
AssociativeBTB::getIndex(ThreadID tid, Addr instPC)
{
    /**
     * Compute the index into the BTB.
     * - Shift PC over by the word offset
     * - Mask the address to use only the specified number of TAG bits
     *   plus the bits to for the set index.
     *
     *  64                          0
     *  | xxx |   TAG    |  Set  |bb|
     *         \_____BTB idx____/
     *
     * The TID will be appended as MSB to the index
     */
    // ((instPC >> instShiftAmt) ^ (uint64_t(tid) << idxBits)) & idxMask;
    return (instPC >> instShiftAmt) & idxMask;
}

bool
AssociativeBTB::valid(ThreadID tid, Addr instPC, BranchClass type)
{
    uint64_t idx = getIndex(tid, instPC);
    BTBEntry * entry = btb.findEntry(idx, /* unused */ false);

    if (entry != nullptr && entry->tid == tid) {
        DPRINTF(BTB, "BTB::%s: hit PC: %#x, idx:%#x \n",
                     __func__, instPC, idx);
        return true;
    }

    return false;
}

// @todo Create some sort of return struct that has both whether or not the
// address is valid, and also the address.  For now will just use addr = 0 to
// represent invalid entry.
const PCStateBase *
AssociativeBTB::lookup(ThreadID tid, Addr instPC, BranchClass type)
{
    stats.lookups++;
    if (type != BranchClass::NoBranch) {
        stats.lookupType[type]++;
    }


    uint64_t idx = getIndex(tid, instPC);
    BTBEntry * entry = btb.findEntry(idx, /* unused */ false);

    if (entry != nullptr && entry->tid == tid) {
        DPRINTF(BTB, "BTB::%s: hit PC: %#x, idx:%#x \n",
                     __func__, instPC, idx);

        btb.accessEntry(entry);
        return entry->target;
    }
    stats.misses++;
    if (type != BranchClass::NoBranch) {
        stats.missType[type]++;
    }
    return nullptr;
}

const StaticInstPtr
AssociativeBTB::lookupInst(ThreadID tid, Addr instPC)
{
    uint64_t idx = getIndex(tid, instPC);
    BTBEntry * entry = btb.findEntry(idx, /* unused */ false);

    if (entry != nullptr && entry->tid == tid) {
        DPRINTF(BTB, "BTB::%s: hit PC: %#x, idx:%#x \n",
                     __func__, instPC, idx);

        // The access was done earlier so it should not be necessary right?
        // btb.accessEntry(entry);
        return entry->inst;
    }
    return nullptr;
}

void
AssociativeBTB::update(ThreadID tid, Addr instPC,
                    const PCStateBase &target,
                    BranchClass type, StaticInstPtr inst)
{
    stats.updates++;
    if (type != BranchClass::NoBranch) {
        stats.updateType[type]++;
    }

    uint64_t idx = getIndex(tid, instPC);
    BTBEntry * entry = btb.findEntry(idx, /* unused */ false);

    if (entry != nullptr && entry->tid == tid) {
        DPRINTF(BTB, "BTB::%s: Updated existing entry. PC:%#x, idx:%#x \n",
                     __func__, instPC, idx);
        btb.accessEntry(entry);
    } else {
        DPRINTF(BTB, "BTB::%s: Replay entry. PC:%#x, idx:%#x \n",
                     __func__, instPC, idx);
        stats.evictions++;
        entry = btb.findVictim(idx);
        assert(entry != nullptr);
        btb.insertEntry(idx, false, entry);
    }

    entry->tid = tid;
    set(entry->target, &target);
    entry->inst = inst;
    // entry->target = &target;
}

} // namespace branch_prediction
} // namespace gem5
