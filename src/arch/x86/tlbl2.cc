/*
 * Copyright (c) 2007-2008 The Hewlett-Packard Development Company
 * All rights reserved.
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
 *
 * Authors: Gabe Black
 */

#include "arch/x86/tlbl2.hh"

#include <cstring>
#include <memory>

// #include "arch/generic/mmapped_ipr.hh"
#include "arch/x86/faults.hh"
#include "arch/x86/insts/microldstop.hh"
#include "arch/x86/pagetable_walker.hh"
#include "arch/x86/pseudo_inst_abi.hh"
#include "arch/x86/regs/misc.hh"
#include "arch/x86/regs/msr.hh"
#include "arch/x86/x86_traits.hh"
#include "base/trace.hh"
#include "cpu/thread_context.hh"
#include "debug/TLB.hh"
#include "mem/packet_access.hh"
#include "mem/page_table.hh"
#include "mem/request.hh"
#include "sim/full_system.hh"
#include "sim/process.hh"
#include "sim/pseudo_inst.hh"

namespace gem5
{

namespace X86ISA {

bool isPow2(uint32_t val)
{
    //https://stackoverflow.com/a/108360/754562
    return val && !(val & (val-1));
}

TLBL2::TLBL2(const Params &p)
    : TLB(p), configAddress(0),
      size_l1_4k(p.size_l1_4k),
      size_l1_2m(p.size_l1_2m),
      size_l2(p.size_l2),

      assoc_l1_4k(p.assoc_l1_4k),
      assoc_l1_2m(p.assoc_l1_2m),
      assoc_l2(p.assoc_l2),

      set_l1_4k(size_l1_4k / assoc_l1_4k),
      set_l1_2m(size_l1_2m / assoc_l1_2m),
      set_l2(size_l2 / assoc_l2),

      walk_lat(p.fixed_l2_miss_latency),
      l2_access_lat(p.l2_hit_latency),

      force_4KB_page(p.force_4k),

      tlb_l1_4k(set_l1_4k),
      tlb_l1_2m(set_l1_2m),
      tlb_l2(set_l2),
      walkCompleteEvent([this]{completeTranslation();}, name()),
      stats(this)

      //lruSeq(0)
{
    if (!size_l1_4k || !size_l1_2m || !size_l2)
        fatal("TLBL2s must have a non-zero size.\n");

    //Checks!
    if (!isPow2(set_l1_4k)) {
        fatal("TLBL2 L1 4K TLB set size is not pow2\n");
    }
    if (!isPow2(set_l1_2m)) {
        fatal("TLBL2 L1 2M TLB set size is not pow2\n");
    }
    if (!isPow2(set_l2)) {
        fatal("TLBL2 L2 TLB set size is not pow2\n");
    }

    // Build TLB tables
    for (int x = 0; x < set_l1_4k; x++) {
        tlb_l1_4k.at(x) = std::vector<TlbEntry*>(assoc_l1_4k, NULL);
    }
    for (int x = 0; x < set_l1_2m; x++) {
        tlb_l1_2m.at(x) = std::vector<TlbEntry*>(assoc_l1_2m, NULL);
    }
    for (int x = 0; x < set_l2; x++) {
        tlb_l2.at(x) = std::vector<TlbEntry*>(assoc_l2, NULL);
    }

    walker = p.walker;
    walker->setTLB(this);
    walker->setLatAndAction(walk_lat, PageWalk_4K);

    transInflight = false;
    inflight_tc = NULL;
    inflight_trans = NULL;
}

//Inserts into specific TLB
TlbEntry *
TLBL2::insertInto(Addr vpn, const TlbEntry &entry, TLBType dest)
{
    std::vector<TlbEntry *> *tlb_way = NULL;
    int idx = getIndex(vpn, dest);

    switch (dest) {
        case L1_4K:
            tlb_way = &tlb_l1_4k.at(idx);
            assert(!entry.largepage);
            break;

        case L1_2M:
            tlb_way = &tlb_l1_2m.at(idx);
            assert(entry.largepage);
            break;

        case L2_4K:
        case L2_2M:
            tlb_way = &tlb_l2.at(idx);
            break;
        default:
            break;
    }

    // Check if there is emtpy entry, else get the oldest
    uint64_t oldest_seq = lruSeq;
    std::vector<TlbEntry *>::iterator oldest_it;
    for (auto it = tlb_way->begin(); it != tlb_way->end(); it++) {
        if (!*it) {
            oldest_it = it;
            break;
        }
        if (oldest_seq >= (*it)->lruSeq) {
            oldest_it = it;
            oldest_seq = (*it)->lruSeq;
        }
    }

    TlbEntry *ent;
    if (freeList.empty()) {
        ent = new TlbEntry();
    } else {
        ent = freeList.front();
        freeList.pop_front();
    }

    ent->paddr = entry.paddr;
    ent->vaddr = entry.vaddr;
    ent->logBytes = entry.logBytes;
    ent->writable = entry.writable;
    ent->user = entry.user;
    ent->uncacheable = entry.uncacheable;
    ent->global = entry.global;
    ent->patBit = entry.patBit;
    ent->noExec = entry.noExec;
    ent->largepage = entry.largepage;
    ent->lruSeq = nextSeq();

    *oldest_it = ent;


    return ent;
}

//Inserts into all applicable levels
TlbEntry *
TLBL2::insert(Addr vpn, const TlbEntry &entry)
{
    if (entry.largepage) {
        insertInto(vpn, entry, L1_2M);
        return insertInto(vpn, entry, L2_2M);
    } else {
        insertInto(vpn, entry, L1_4K);
        return insertInto(vpn, entry, L2_4K);
    }
}

int TLBL2::getIndex(Addr va, TLBType type)
{
    int idx = 0;
    switch(type){
        case L1_4K:
            idx = va >> 12;
            idx &= set_l1_4k-1;
            assert(idx >= 0 && idx < set_l1_4k);
            break;

        case L2_4K:
            idx = va >> 12;
            idx &= set_l2-1;
            assert(idx >= 0 && idx < set_l2);
            break;

        case L1_2M:
            idx = va >> 21;
            idx &= set_l1_2m-1;
            assert(idx >= 0 && idx < set_l1_2m);
            break;

        case L2_2M:
            idx = va >> 21;
            idx &= set_l2-1;
            assert(idx >= 0 && idx < set_l2);
            break;
        default:
            fatal("Not implemented");
    }
    return idx;
}

TlbEntry *
TLBL2::lookup(Addr va, int &delay_cycles, bool update_lru)
{
    TlbEntry *entry = NULL;
    Addr vpn_4k = va & ~((1UL << 12)-1);
    TLBType hitLevel = Miss;
    int delays = 0;

    // Lookup L1_4k
    int idx = getIndex(vpn_4k, L1_4K);
    for (TlbEntry *ent: tlb_l1_4k.at(idx)) {
       if (!ent) //empty
           continue;
       if (ent->vaddr == vpn_4k) {
           entry = ent;
           hitLevel = L1_4K;
           break;
       }
    }

    Addr vpn_2m = va & ~((1UL << 21)-1);
    if (!entry && !force_4KB_page) {
        // Lookup L1_2m
        int idx = getIndex(vpn_2m, L1_2M);
        for (TlbEntry *ent: tlb_l1_2m.at(idx)) {
            if (!ent) //empty
                continue;
            if (ent->vaddr == vpn_2m) {
                entry = ent;
                hitLevel = L1_2M;
                break;
            }
        }
    }

    if (!entry) {
        // Lookup L2 4K
        int idx = getIndex(vpn_4k, L2_4K);
        for (TlbEntry *ent: tlb_l2.at(idx)) {
            if (!ent) //empty
                continue;
            if (ent->vaddr == vpn_4k && !ent->largepage) {
                entry = ent;
                hitLevel = L2_4K;
                delays += l2_access_lat;
                break;
            }
        }

        if (entry) { // L2 4K hit
            //Fill L1!
            insertInto(vpn_4k, *entry, L1_4K);
        } else if (!force_4KB_page) {
            // Lookup L2 2M
            idx = getIndex(vpn_2m, L2_2M);
            for (TlbEntry *ent: tlb_l2.at(idx)) {
                if (!ent) //empty
                    continue;
                if (ent->vaddr == vpn_2m && ent->largepage) {
                    entry = ent;
                    hitLevel = L2_2M;
                    delays += l2_access_lat;
                    break;
                }
            }
            if (entry) { //L2 2M hit
                //Fill L1!
                insertInto(vpn_2m, *entry, L1_2M);
                //TODO we may need to move this into the page walker...
            }
        }
    }

    if (update_lru) { // Also update counters
        if (entry) {
            //Update LRU
            entry->lruSeq = nextSeq();

            switch(hitLevel) {
                case L1_4K:
                    stats.l1_4k_hits++;
                    break;
                case L1_2M:
                    stats.l1_2m_hits++;
                    break;
                case L2_4K:
                    stats.l1_misses++;
                    stats.l2_4k_hits++;
                    break;
                case L2_2M:
                    stats.l1_misses++;
                    stats.l2_2m_hits++;
                    break;
                default:
                    break;
            }
            assert(hitLevel != Miss);
        } else {
            assert(hitLevel == Miss);
            stats.l1_misses++;
            stats.l2_misses++;
        }
    }
    delay_cycles = delays;
    return entry;
}

void
TLBL2::flushAll()
{
    DPRINTF(TLB, "Invalidating all entries.\n");
    for (int i = 0; i < set_l1_4k; i++) {
        for (auto it = tlb_l1_4k.at(i).begin();
                it != tlb_l1_4k.at(i).end();
                it++) {
            if (*it) { // Not NULL)
                freeList.push_back(*it);
                *it = NULL;
            }
        }
    }

    for (int i = 0; i < set_l1_2m; i++) {
        for (auto it = tlb_l1_2m.at(i).begin();
                it != tlb_l1_2m.at(i).end();
                it++) {
            if (*it) { // Not NULL)
                freeList.push_back(*it);
                *it = NULL;
            }
        }
    }

    for (int i = 0; i < set_l2; i++) {
        for (auto it = tlb_l2.at(i).begin();
                it != tlb_l2.at(i).end();
                it++) {
            if (*it) { // Not NULL)
                freeList.push_back(*it);
                *it = NULL;
            }
        }
    }
}

void
TLBL2::setConfigAddress(uint32_t addr)
{
    configAddress = addr;
}

void
TLBL2::flushNonGlobal()
{
    DPRINTF(TLB, "Invalidating all non global entries.\n");
    for (int i = 0; i < set_l1_4k; i++) {
        for (auto it = tlb_l1_4k.at(i).begin();
                it != tlb_l1_4k.at(i).end();
                it++) {
            if (*it && !(*it)->global) {
                freeList.push_back(*it);
                *it = NULL;
            }
        }
    }

    for (int i = 0; i < set_l1_2m; i++) {
        for (auto it = tlb_l1_2m.at(i).begin();
                it != tlb_l1_2m.at(i).end();
                it++) {
            if (*it && !(*it)->global) {
                freeList.push_back(*it);
                *it = NULL;
            }
        }
    }

    for (int i = 0; i < set_l2; i++) {
        for (auto it = tlb_l2.at(i).begin();
                it != tlb_l2.at(i).end();
                it++) {
            if (*it && !(*it)->global) {
                freeList.push_back(*it);
                *it = NULL;
            }
        }
    }
}

//Invalidate TLB entry
void
TLBL2::demapPage(Addr va, uint64_t asn)
{
    //TODO
    TlbEntry *entry = trie.lookup(va);
    if (entry) {
        trie.remove(entry->trieHandle);
        entry->trieHandle = NULL;
        freeList.push_back(entry);
    }
}

namespace
{

Cycles
localMiscRegAccess(bool read, RegIndex regNum,
                   ThreadContext *tc, PacketPtr pkt)
{
    if (read) {
        RegVal data = htole(tc->readMiscReg(regNum));
        assert(pkt->getSize() <= sizeof(RegVal));
        pkt->setData((uint8_t *)&data);
    } else {
        RegVal data = htole(tc->readMiscRegNoEffect(regNum));
        assert(pkt->getSize() <= sizeof(RegVal));
        pkt->writeData((uint8_t *)&data);
        tc->setMiscReg(regNum, letoh(data));
    }
    return Cycles(1);
}

} // anonymous namespace

Fault
TLBL2::translateInt(bool read, RequestPtr req, ThreadContext *tc)
{
    DPRINTF(TLB, "Addresses references internal memory.\n");
    Addr vaddr = req->getVaddr();
    Addr prefix = (vaddr >> 3) & IntAddrPrefixMask;
    if (prefix == IntAddrPrefixCPUID) {
        panic("CPUID memory space not yet implemented!\n");
    } else if (prefix == IntAddrPrefixMSR) {
        vaddr = (vaddr >> 3) & ~IntAddrPrefixMask;

        RegIndex regNum;
        if (!msrAddrToIndex(regNum, vaddr))
            return std::make_shared<GeneralProtection>(0);

        req->setPaddr(req->getVaddr());
        req->setLocalAccessor(
            [read,regNum](ThreadContext *tc, PacketPtr pkt)
            {
                return localMiscRegAccess(read, regNum, tc, pkt);
            }
        );

        return NoFault;
    } else if (prefix == IntAddrPrefixIO) {
        // TODO If CPL > IOPL or in virtual mode, check the I/O permission
        // bitmap in the TSS.

        Addr IOPort = vaddr & ~IntAddrPrefixMask;
        // Make sure the address fits in the expected 16 bit IO address
        // space.
        assert(!(IOPort & ~0xFFFF));
        if (IOPort == 0xCF8 && req->getSize() == 4) {
            req->setPaddr(req->getVaddr());
            req->setLocalAccessor(
                [read](ThreadContext *tc, PacketPtr pkt)
                {
                    return localMiscRegAccess(
                            read, misc_reg::PciConfigAddress, tc, pkt);
                }
            );
        } else if ((IOPort & ~mask(2)) == 0xCFC) {
            req->setFlags(Request::UNCACHEABLE | Request::STRICT_ORDER);
            Addr configAddress =
                tc->readMiscRegNoEffect(misc_reg::PciConfigAddress);
            if (bits(configAddress, 31, 31)) {
                req->setPaddr(PhysAddrPrefixPciConfig |
                        mbits(configAddress, 30, 2) |
                        (IOPort & mask(2)));
            } else {
                req->setPaddr(PhysAddrPrefixIO | IOPort);
            }
        } else {
            req->setFlags(Request::UNCACHEABLE | Request::STRICT_ORDER);
            req->setPaddr(PhysAddrPrefixIO | IOPort);
        }
        return NoFault;
    } else {
        panic("Access to unrecognized internal address space %#x.\n",
                prefix);
    }
}
Fault
TLBL2::finalizePhysical(const RequestPtr &req,
                      ThreadContext *tc, BaseMMU::Mode mode) const
{
    Addr paddr = req->getPaddr();

    if (m5opRange.contains(paddr)) {
        req->setFlags(Request::STRICT_ORDER);
        uint8_t func;
        pseudo_inst::decodeAddrOffset(paddr - m5opRange.start(), func);
        req->setLocalAccessor(
            [func, mode](ThreadContext *tc, PacketPtr pkt) -> Cycles
            {
                uint64_t ret;
                pseudo_inst::pseudoInst<X86PseudoInstABI, true>(tc, func, ret);
                if (mode == BaseMMU::Read)
                    pkt->setLE(ret);
                return Cycles(1);
            }
        );
    } else if (FullSystem) {
        // Check for an access to the local APIC
        LocalApicBase localApicBase =
            tc->readMiscRegNoEffect(misc_reg::ApicBase);
        AddrRange apicRange(localApicBase.base * PageBytes,
                            (localApicBase.base + 1) * PageBytes);

        if (apicRange.contains(paddr)) {
            // The Intel developer's manuals say the below restrictions apply,
            // but the linux kernel, because of a compiler optimization, breaks
            // them.
            /*
            // Check alignment
            if (paddr & ((32/8) - 1))
                return new GeneralProtection(0);
            // Check access size
            if (req->getSize() != (32/8))
                return new GeneralProtection(0);
            */
            // Force the access to be uncacheable.
            req->setFlags(Request::UNCACHEABLE | Request::STRICT_ORDER);
            req->setPaddr(x86LocalAPICAddress(tc->contextId(),
                                              paddr - apicRange.start()));
        }
    }

    return NoFault;
}

Fault
TLBL2::translate(const RequestPtr &req,
        ThreadContext *tc, BaseMMU::Translation *translation,
        BaseMMU::Mode mode, bool &delayedResponse, bool timing)
{
    Request::Flags flags = req->getFlags();
    int seg = flags & SegmentFlagMask;
    bool storeCheck = flags & Request::READ_MODIFY_WRITE;

    delayedResponse = false;

    // If this is true, we're dealing with a request to a non-memory address
    // space.
    if (seg == segment_idx::Ms) {
        return translateInt(mode == BaseMMU::Read, req, tc);
    }

    Addr vaddr = req->getVaddr();
    DPRINTF(TLB, "Translating vaddr %#x.\n", vaddr);

    HandyM5Reg m5Reg = tc->readMiscRegNoEffect(misc_reg::M5Reg);

    const Addr logAddrSize = (flags >> AddrSizeFlagShift) & AddrSizeFlagMask;
    const int addrSize = 8 << logAddrSize;
    const Addr addrMask = mask(addrSize);

    // If protected mode has been enabled...
    if (m5Reg.prot) {
        DPRINTF(TLB, "In protected mode.\n");
        // If we're not in 64-bit mode, do protection/limit checks
        if (m5Reg.mode != LongMode) {
            DPRINTF(TLB, "Not in long mode. Checking segment protection.\n");

            // CPUs won't know to use CS when building fetch requests, so we
            // need to override the value of "seg" here if this is a fetch.
            if (mode == BaseMMU::Execute)
                seg = segment_idx::Cs;

            SegAttr attr = tc->readMiscRegNoEffect(misc_reg::segAttr(seg));
            // Check for an unusable segment.
            if (attr.unusable) {
                DPRINTF(TLB, "Unusable segment.\n");
                return std::make_shared<GeneralProtection>(0);
            }
            bool expandDown = false;
            if (seg >= segment_idx::Es && seg <= segment_idx::Hs) {
                if (!attr.writable && (mode == BaseMMU::Write || storeCheck)) {
                    DPRINTF(TLB, "Tried to write to unwritable segment.\n");
                    return std::make_shared<GeneralProtection>(0);
                }
                if (!attr.readable && mode == BaseMMU::Read) {
                    DPRINTF(TLB, "Tried to read from unreadble segment.\n");
                    return std::make_shared<GeneralProtection>(0);
                }
                expandDown = attr.expandDown;

            }
            Addr base = tc->readMiscRegNoEffect(misc_reg::segBase(seg));
            Addr limit = tc->readMiscRegNoEffect(misc_reg::segLimit(seg));
            Addr offset;
            if (mode == BaseMMU::Execute)
                offset = vaddr - base;
            else
                offset = (vaddr - base) & addrMask;
            Addr endOffset = offset + req->getSize() - 1;
            if (expandDown) {
                DPRINTF(TLB, "Checking an expand down segment.\n");
                warn_once("Expand down segments are untested.\n");
                if (offset <= limit || endOffset <= limit)
                    return std::make_shared<GeneralProtection>(0);
            } else {
                if (offset > limit || endOffset > limit) {
                    DPRINTF(TLB, "Segment limit check failed, "
                            "offset = %#x limit = %#x.\n", offset, limit);
                    return std::make_shared<GeneralProtection>(0);
                }
            }
        }
        if (m5Reg.submode != SixtyFourBitMode && addrSize != 64)
            vaddr &= mask(32);
        // If paging is enabled, do the translation.


        int delays = 0;
        if (m5Reg.paging) {
            DPRINTF(TLB, "Paging enabled.\n");
            // The vaddr already has the segment base applied.
            TlbEntry *entry = lookup(vaddr, delays);

            if (!entry) {
                DPRINTF(TLB, "Handling a TLBL2 miss for "
                        "address %#x at pc %#x.\n",
                        vaddr, tc->pcState().instAddr());

                if (FullSystem) {
                    Fault fault = walker->start(tc, translation, req, mode);
                    if (timing || fault != NoFault) {
                        // This gets ignored in atomic mode.
                        delayedResponse = true;
                        return fault;
                    }
                    entry = lookup(vaddr, delays);
                    assert(entry);
                } else {
                    Process *p = tc->getProcessPtr();
                    const EmulationPageTable::Entry *pte =
                        p->pTable->lookup(vaddr);
                    if (!pte) {
                        return std::make_shared<PageFault>(vaddr, true, mode,
                                                           true, false);
                    } else {
                        if (timing) {
                            TLBWalkerAction action = PageWalk_4K;

                            if (pte->isLargePageEntry()) {
                                if (force_4KB_page)
                                    action = PageWalk_2M_force_4K;
                                else
                                    action = PageWalk_2M;
                            }

                            walker->setLatAndAction(walk_lat, action);
                            Fault fault = walker->start(tc, translation, req,
                                                        mode);
                            if (timing || fault != NoFault) {
                                    // This gets ignored in atomic mode.
                                    delayedResponse = true;
                                    return fault;
                            }
                        } else {
                            //TODO CHP will this work?
                            Addr alignedVaddr;
                            if (pte->isLargePageEntry()) {
                                alignedVaddr = p->pTable->
                                    largePageAlign(vaddr);
                            } else {
                                alignedVaddr = p->pTable->pageAlign(vaddr);
                            }
                            DPRINTF(TLB, "Mapping %s %#x to %#x\n",
                                    alignedVaddr, pte->isLargePageEntry() ?
                                    "largepage" : "",
                                    pte->paddr);
                            entry = insert(alignedVaddr, TlbEntry(
                                p->pTable->pid(), alignedVaddr, pte->paddr,
                                pte->flags & EmulationPageTable::Uncacheable,
                                pte->flags & EmulationPageTable::ReadOnly,
                                pte->isLargePageEntry()));
                        }
                    }
                }
            } else if (timing && delays) {
                TLBWalkerAction action = Access_L2;
                walker->setLatAndAction(delays, action);
                Fault fault = walker->start(tc, translation, req, mode);
                delayedResponse = true;
                return fault;
            }

            DPRINTF(TLB, "Entry found with paddr %#x, "
                    "doing protection checks.\n", entry->paddr);
            // Do paging protection checks.
            bool inUser = m5Reg.cpl == 3 && !(flags & CPL0FlagBit);
            CR0 cr0 = tc->readMiscRegNoEffect(misc_reg::Cr0);
            bool badWrite = (!entry->writable && (inUser || cr0.wp));
            if ((inUser && !entry->user) ||
                (mode == BaseMMU::Write && badWrite)) {
                // The page must have been present to get into the TLBL2 in
                // the first place. We'll assume the reserved bits are
                // fine even though we're not checking them.
                return std::make_shared<PageFault>(vaddr, true, mode, inUser,
                                                   false);
            }
            if (storeCheck && badWrite) {
                // This would fault if this were a write, so return a page
                // fault that reflects that happening.
                return std::make_shared<PageFault>(
                    vaddr, true, BaseMMU::Write, inUser, false);
            }

            Addr paddr = entry->paddr | (vaddr & mask(entry->logBytes));
            DPRINTF(TLB, "Translated %#x -> %#x.\n", vaddr, paddr);
            req->setPaddr(paddr);
            if (entry->uncacheable)
                req->setFlags(Request::UNCACHEABLE | Request::STRICT_ORDER);
        } else {
            //Use the address which already has segmentation applied.
            DPRINTF(TLB, "Paging disabled.\n");
            DPRINTF(TLB, "Translated %#x -> %#x.\n", vaddr, vaddr);
            req->setPaddr(vaddr);
        }
    } else {
        // Real mode
        DPRINTF(TLB, "In real mode.\n");
        DPRINTF(TLB, "Translated %#x -> %#x.\n", vaddr, vaddr);
        req->setPaddr(vaddr);
    }

    return finalizePhysical(req, tc, mode);
}

Fault
TLBL2::translateAtomic(const RequestPtr &req, ThreadContext *tc,
    BaseMMU::Mode mode)
{
    bool delayedResponse;
    return TLBL2::translate(req, tc, NULL, mode, delayedResponse, false);
}

void
TLBL2::translateTiming(const RequestPtr &req, ThreadContext *tc,
    BaseMMU::Translation *translation, BaseMMU::Mode mode)
{
    bool delayedResponse;
    assert(translation);
    Fault fault =
        TLBL2::translate(req, tc, translation, mode, delayedResponse, true);
    if (!delayedResponse)
        translation->finish(fault, req, tc, mode);
    else
        translation->markDelayed();
}

void
TLBL2::completeTranslation()
{
    inflight_trans->finish(NoFault, inflight_req, inflight_tc, inflight_mode);
    inflight_trans = NULL;
    inflight_tc = NULL;
    transInflight = false;
    DPRINTF(TLB, "Miss was serviced.\n");
}

Walker *
TLBL2::getWalker()
{
    return walker;
}


TLBL2::TlbL2Stats::TlbL2Stats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(l1_4k_hits, statistics::units::Count::get(),
             "TLBL2 L1 TLB 4KB hits"),
    ADD_STAT(l1_2m_hits, statistics::units::Count::get(),
             "TLB accesses on write requests"),
    ADD_STAT(l1_misses, statistics::units::Count::get(),
             "TLB accesses on write requests"),
    ADD_STAT(l2_4k_hits, statistics::units::Count::get(),
             "TLB accesses on write requests"),
    ADD_STAT(l2_2m_hits, statistics::units::Count::get(),
             "TLB accesses on write requests"),
    ADD_STAT(l2_misses, statistics::units::Count::get(),
             "TLB accesses on write requests"),
    ADD_STAT(l2_access_cycles, statistics::units::Count::get(),
             "TLB accesses on write requests"),
    ADD_STAT(walkCycles, statistics::units::Count::get(),
             "TLB accesses on write requests"),
    ADD_STAT(walks, statistics::units::Count::get(),
             "TLB accesses on write requests"),
    ADD_STAT(coalescedWalks, statistics::units::Count::get(),
             "TLB accesses on write requests"),
    ADD_STAT(squashedWalks, statistics::units::Count::get(),
             "TLB accesses on write requests")
{
}

// void
// TLBL2::regStats()
// {
//     using namespace Stats;
//     BaseTLB::regStats();

//     l1_4k_hits
//         .name(name() + ".l1_4k_hits")
//         .desc("TLBL2 L1 TLB 4KB hits");

//     l1_2m_hits
//         .name(name() + ".l1_2m_hits")
//         .desc("TLBL2 L1 TLB 2MB hits");

//     l1_misses
//         .name(name() + ".l1_misses")
//         .desc("TLBL2 L1 TLB misses");


//     l2_4k_hits
//         .name(name() + ".l2_4k_hits")
//         .desc("TLBL2 L2 TLB 4KB hits");

//     l2_2m_hits
//         .name(name() + ".l2_2m_hits")
//         .desc("TLBL2 L2 TLB 2MB hits");

//     l2_misses
//         .name(name() + ".l2_misses")
//         .desc("TLBL2 L2 TLB misses");

//     l2_access_cycles
//         .name(name() + ".l2_access_cycles")
//         .desc("L2 TLB access latency");

//     walkCycles
//         .name(name() + ".walkCycles")
//         .desc("Walk cycles from tlbl2 misses");

//     walks
//         .name(name() + ".walks")
//         .desc("Total number of walks from tlbl2 misses");

//     squashedWalks
//         .name(name() + ".squashedWalks")
//         .desc("Total number of squashed walks from tlbl2 misses");

//     coalescedWalks
//         .name(name() + ".coalescedWalks")
//         .desc("Total number of coalesced walks from tlbl2 walk handling");
// }

//void
//TLBL2::serialize(CheckpointOut &cp) const
//{
//    // Only store the entries in use.
//    uint32_t _size = size_l1_4k - freeList.size();
//    SERIALIZE_SCALAR(_size);
//    SERIALIZE_SCALAR(lruSeq);
//
//    uint32_t _count = 0;
//    for (uint32_t x = 0; x < size_l1_4k; x++) {
//        if (tlb_l1_4k[x].trieHandle != NULL)
//            tlb_l1_4k[x].serializeSection(cp, csprintf("Entry%d", _count++));
//    }
//}
//
//void
//TLBL2::unserialize(CheckpointIn &cp)
//{
//    // Do not allow to restore with a smaller tlb.
//    uint32_t _size;
//    UNSERIALIZE_SCALAR(_size);
//    if (_size > size_l1_4k) {
//        fatal("TLBL2 size less than the one in checkpoint!");
//    }
//
//    UNSERIALIZE_SCALAR(lruSeq);
//
//    for (uint32_t x = 0; x < _size; x++) {
//        TlbEntry *newEntry = freeList.front();
//        freeList.pop_front();
//
//        newEntry->unserializeSection(cp, csprintf("Entry%d", x));
//        newEntry->trieHandle = trie.insert(newEntry->vaddr,
//            TlbEntryTrie::MaxBits - newEntry->logBytes, newEntry);
//    }
//}

Port *
TLBL2::getTableWalkerPort()
{
    return &walker->getPort("port");
}

} // namespace X86ISA
} // namespace gem5


// X86ISA::TLBL2 *
// X86TLBL2Params::create()
// {
//     return new X86ISA::TLBL2(this);
// }
