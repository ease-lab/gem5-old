/*
 * Copyright (c) 2005 The Regents of The University of Michigan
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

/**
 * @file
 * Describes the PIF prefetcher implementation of Champsim
 */

#include "mem/cache/prefetch/pif2.hh"

#include "params/PIF2Prefetcher.hh"

// namespace gem5
// {

// GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
// namespace prefetch
// {

// Fake::Fake(const FakePrefetcherParams &p)
//     : Queued(p), degree(p.degree)
// {

// }

// void
// Fake::calculatePrefetch(const PrefetchInfo &pfi,
//     std::vector<AddrPriority> &addresses)
// {
//     Addr blkAddr = blockAddress(pfi.getAddr());

//     for (int d = 1; d <= degree; d++) {
//         Addr newAddr = blkAddr + d*(blkSize);
//         addresses.push_back(AddrPriority(newAddr,0));
//     }
// }

// } // namespace prefetch
// } // namespace gem5

#include "mem/cache/prefetch/pif2.hh"

#include <utility>

#include "debug/HWPrefetch.hh"
#include "mem/cache/base.hh"
#include "params/PIF2Prefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

PIF2::PIF2(const PIF2PrefetcherParams &p)
    : Base(p),
        latency(p.latency),
        regionSize(8),
        spatialOffset(0), // position of the first block accessed in a region
        theLookahead(5),
        theTrackerSize(12),
        theCompactorSize(18), // 6;
        theStreamCount(3),
        theBlockSize(64),
        theBlockOffsetSize((int)log2(theBlockSize)),
        theBlockMask(~(theBlockSize - 1)),
        infinite(p.infinite),
        cacheSnoop(p.cache_snoop),
        next_ready_time(0),
        pfq(),
        history(nullptr),
        streamTracker(nullptr),
        compactor(),
        listenersPC(),
        statsPIF2(this)
{
    resetState();
    // // Calculate the size of PIF
    // // Size per entry is: 34 bit trigger address + succ. + predecessor bits
    // auto entry_size = 34 + precSize + succSize;
    // auto total_size = (maxCompactorEntries
    //                 + p.stream_address_buffer_entries
    //                 + p.history_buffer_size) * entry_size
    //                 + p.index_entries * (34+15);

    // warn("PIF Prefetcher with: Index size: %d entries => %dkB"
    //     "Hist: %d entries => %dk ==> total %dk",
    //     p.index_entries, p.index_entries*(34+15)/8/1024,
    //     p.history_buffer_size, p.history_buffer_size * entry_size/8/1024,
    //     total_size /8/1024);
}


void
PIF2::resetState()
{
    // Clear everything if already allocated by deleting
    if (history)
        delete history;
    if (streamTracker)
        delete streamTracker;

    // Now initialize
    if (infinite) {
        DPRINTF(HWPrefetch, "PIF: Create infinite History\n");
        history = new PIFInfiniteHistory(*this);
    } else {

// #if defined(PIF_CIRC)
//         history = new PIFCircularHistory(cfg.HistorySize);
// #elif defined(PIF_CIRC_IND)
        int histSize = 16 * 1024;
        int indexSets = 2 * 512;
        int indexAssoc = 4;

        DPRINTF(HWPrefetch, "PIF: Create History size: %d entries, "
                    "Index size: %d sets, assoc: %d\n", histSize,
                                indexSets, indexAssoc);
        history = new PIFCircularHistoryLimitedIndex(*this, histSize,
                                indexSets, indexAssoc);
// #endif
    }

    streamTracker = new StreamTracker(theStreamCount, theTrackerSize,
                                    theLookahead);
    for (int i = 0; i < theCompactorSize; i++) {
        compactor.push_back(StreamEntry(i + 1, false));
    }
}


void
PIF2::notifyRetiredInst(const Addr pc)
{
    statsPIF2.pifNRetInst++;
    const uint64_t inst_blk = blockAddress(pc);

    // DPRINTF(HWPrefetch, "PIF (N): Notify PC: %#x Inst. Blk: %#x\n",
    //         inst_blk, inst_blk);

    bool isHead = false;
    bool prefetched = false;

    //	optional<Range> aRange = streamTracker->lookup(inst_blk, prefetched);
    std::pair<Range, bool> aRange = streamTracker->lookup(inst_blk,
                                                            prefetched);
    if (!aRange.second) {
        // Pointer lookup
        statsPIF2.pifQIndexReset++;
        uint64_t ptr = history->getPtr(inst_blk);

        isHead = true;
        if (ptr) {
            statsPIF2.pifQIndexResetHit++;
            DPRINTF(HWPrefetch, "PIF (Q): Index Reset: %#x Hit\n", inst_blk);

            aRange.first = streamTracker->allocate(ptr);
            aRange.second = true;
        } else {
            statsPIF2.pifQIndexResetMiss++;
            DPRINTF(HWPrefetch, "PIF (Q): Index Reset: %#x Miss\n", inst_blk);
        }
    } else {
        // We hit in the SAB
        statsPIF2.pifQSABHits++;
        DPRINTF(HWPrefetch, "PIF (Q): Inst. Blk: %#x Hit in SAB\n", inst_blk);

        if (prefetched) {
        }
    }

    if (aRange.second && aRange.first.theLength) {
        DPRINTF(HWPrefetch, "PIF (Q): Found stream with length %d\n",
                                     aRange.first.theLength);
        for (int i = 0; i < aRange.first.theLength; i++) {
    // History read
    //		optional<StreamEntry> aPrefetchEntry = history->read(*aRange);
            std::pair<StreamEntry, bool> aPrefetchEntry =
                                    history->read(aRange.first);
            if (!aPrefetchEntry.second)
                break;
            streamTracker->push_back(aRange.first.thePtr,
                                                aPrefetchEntry.first);
            uint64_t aPrefetchAddress = aPrefetchEntry.first.theRegionBase;
            for (int j = 0; j < regionSize; j++) {
                // enqueue the prefetch addresses
                if (aPrefetchEntry.first.bits[j]) {

                    bool is_recently_prefetched = false;
                    std::list<PFQEntry>::iterator it = std::find(pfq.begin(),
                                                pfq.end(), aPrefetchAddress);
                    if (it == pfq.end()) {
                        std::deque<PFQEntry>::iterator it = std::find(
                                            recent_prefetches.begin(),
                                            recent_prefetches.end(),
                                            aPrefetchAddress);
                        if (it != recent_prefetches.end())
                        {
                            is_recently_prefetched = true;
                            DPRINTF(HWPrefetch, "Already in is_recently_"
                                                            "prefetched\n");
                            statsPIF2.pfPFQHit++;
                        }
                    }
                    else
                    {
                        is_recently_prefetched = true;
                        DPRINTF(HWPrefetch, "Already in prefetch_queue\n");
                    }

                    if (is_recently_prefetched) {
                        continue;
                    }

                    statsPIF2.pfIdentified++;

                    // Create the packet for this address
                    DPRINTF(HWPrefetch, "Create packet for VA: %#x\n",
                                                            aPrefetchAddress);
                    PacketPtr pkt = createPktFromVaddr(aPrefetchAddress);

                    if (!pkt) {
                        DPRINTF(HWPrefetch, "Fail to create packet\n");
                        continue;
                    }

                    DPRINTF(HWPrefetch, "Successfully created packet\n");
                    statsPIF2.pfPacketsCreated++;

                    if (cacheSnoop && (inCache(pkt->getAddr(), true)
                                || (inMissQueue(pkt->getAddr(), true)))) {
                        statsPIF2.pfInCache++;
                        continue;
                    }

                    Tick t = curTick() + clockPeriod() * latency;
                    pfq.push_back(PFQEntry(aPrefetchAddress,pkt,t));
                }
                aPrefetchAddress += 64;
            }
            history->incrementPointer(aRange.first);
        }
    }

    // Pass through the compactor
    bool evict = true;
    for (list<StreamEntry>::iterator i = compactor.begin();
                                    i != compactor.end(); i++)
    {
        bool prefetched;
        if (i->inRange(inst_blk, prefetched))
        {
            int index = i->getIndex(inst_blk);
            i->bits.set(index);
            evict = false;
            break;
        }
    }
    if (evict)
    {
        StreamEntry victim = compactor.front();
        // remove the least recent address from the compactor (FIFO order)
        compactor.pop_front();
        isHead = true;
        compactor.push_back(StreamEntry(inst_blk, isHead));
        // add the new address to the compactor

        // History write
        DPRINTF(HWPrefetch, "Write to history buffer\n");
        statsPIF2.pifNHistWrites++;
        history->record(victim);
    }
}

////////////////////////////////////////////

PacketPtr
PIF2::getPacket()
{
    if (pfq.size() == 0)
    {
        return nullptr;
    }
    PacketPtr pkt = pfq.front().pkt;
    recent_prefetches.push_back(pfq.front());
    if (recent_prefetches.size() > MAX_RECENT_PFETCH) {
        recent_prefetches.pop_front();
    }
    pfq.pop_front();

    DPRINTF(HWPrefetch, "Issue Prefetch to : %#x\n", pkt->getAddr());
    prefetchStats.pfIssued++;
    issuedPrefetches += 1;
    return pkt;
}

Addr
PIF2::translateFunctional(Addr vaddr, ThreadContext* tc)
{
    bool can_cross_page = (tlb != nullptr);
    Addr paddr = 0;

    if (can_cross_page)
    {

        // RequestPtr tl_req = createPrefetchRequest(orig_addr, pfi, pkt);
        RequestPtr tl_req = std::make_shared<Request>(
            vaddr, blkSize, 0, requestorId);
        // translation_req->setFlags(Request::PREFETCH);
        if (tc == nullptr) {
            tc = cache->system->threads[0];
        }

        DPRINTF(HWPrefetch, "%s Try trans of pc %#x  "
                            "\n",
                tlb->name(),
                tl_req->getVaddr());
        Fault fault = tlb->translateFunctional(tl_req, tc, BaseMMU::Read);
        if (fault == NoFault)
        {
            DPRINTF(HWPrefetch, "%s Translation of vaddr %#x succeeded: "
                                "paddr %#x \n",
                    tlb->name(),
                    tl_req->getVaddr(),
                    tl_req->getPaddr());
            paddr = tl_req->getPaddr();
        }
    }
    return paddr;
}

bool
PIF2::translateFunctional(RequestPtr req)
{
    if (tlb == nullptr) {
        return false;
    }

    auto tc = cache->system->threads[req->contextId()];

    DPRINTF(HWPrefetch, "%s Try trans of pc %#x\n",
                                tlb->name(), req->getVaddr());
    Fault fault = tlb->translateFunctional(req, tc, BaseMMU::Read);
    if (fault == NoFault) {
        DPRINTF(HWPrefetch, "%s Translation of vaddr %#x succeeded: "
                        "paddr %#x \n", tlb->name(), req->getVaddr(),
                        req->getPaddr());
        return true;
    }
    return false;
}

/////////////////////////////

void
PIF2::PrefetchListenerPC::notify(const Addr &pc)
{
    parent.notifyRetiredInst(pc);
}

void
PIF2::addEventProbeRetiredInsts(SimObject *obj, const char *name)
{
    warn("Register HWP event probe %s for obj: %s", name, obj->name());
    ProbeManager *pm(obj->getProbeManager());
    listenersPC.push_back(new PrefetchListenerPC(*this, pm, name));
}

PIF2::PIF2Stats::PIF2Stats(statistics::Group *parent)
    : statistics::Group(parent),
        ADD_STAT(pifQueries, statistics::units::Count::get(),
                "number of queries to the pref. (accesses)"),
        ADD_STAT(pifQHasBeenPref, statistics::units::Count::get(),
                "number of accesses that hit on a prefetch."),
        ADD_STAT(pifQSABHits, statistics::units::Count::get(),
                "number of SAB its"),
        ADD_STAT(pifQIndexReset, statistics::units::Count::get(),
                "number misses in the index after reindexing"),
        ADD_STAT(pifQIndexResetMiss, statistics::units::Count::get(),
                "number misses in the index after reindexing"),
        ADD_STAT(pifQIndexResetHit, statistics::units::Count::get(),
                "number hits in the index after reindexing"),
        ADD_STAT(pfPFQHit, statistics::units::Count::get(),
             "number of redundant prefetches already in prefetch queue"),

        ADD_STAT(pifNRetInst, statistics::units::Count::get(),
                "number of evictions from the index"),
        ADD_STAT(pifNIndexInsert, statistics::units::Count::get(),
                "number of evictions from the index"),
        ADD_STAT(pifNHistWrites, statistics::units::Count::get(),
                "number of writes to the history buffer"),
        ADD_STAT(pifNHitSpatial, statistics::units::Count::get(),
                "number of prefetches hit in temp. compactor"),
        ADD_STAT(pifNHitTemporal, statistics::units::Count::get(),
                "number of prefetches hit in temp. compactor"),
        ADD_STAT(pfIdentified, statistics::units::Count::get(),
                "number of prefetches identified."),
        ADD_STAT(pfInCache, statistics::units::Count::get(),
                "number of prefetches hit in in cache"),
        ADD_STAT(pfInCachePrefetched, statistics::units::Count::get(),
                "number of prefetches hit in cache but prefetched"),
        ADD_STAT(pfPacketsCreated, statistics::units::Count::get(),
                "number of prefetch packets created")
{
}

} // namespace prefetch
} // namespace gem5
