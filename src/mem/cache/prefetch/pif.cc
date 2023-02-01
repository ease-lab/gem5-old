/**
 * Copyright (c) 2019 Metempsy Technology Consulting
 * Copyright (c) 2022 University of Edinburgh
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

#include "mem/cache/prefetch/pif.hh"

#include <utility>

#include "debug/HWPrefetch.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "params/PIFPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

PIF::PIF(const PIFPrefetcherParams &p)
    : Queued(p),
      precSize(p.prec_spatial_region_bits),
      succSize(p.succ_spatial_region_bits),
      maxCompactorEntries(p.compactor_entries),
      useBlkAddr(p.use_blk_addr),
      predictOnRetiredPC(p.pred_on_retired_pc),
      sabSize(p.stream_address_buffer_entries),
      lookahead(p.lookahead), compactor_lru(p.compactor_lru),
      historyBuffer(p.history_buffer_size),
      index(p.index_assoc, p.index_entries, p.index_indexing_policy,
            p.index_replacement_policy),
      cpu(p.cpu),
      statsPIF(this)
{
    fatal_if(lookahead >= sabSize, "We cannot look further away then the size"
            " of the SAB. Also 0 lookahead makes no sense.");
    // Calculate the size of PIF
    // Size per entry is: 34 bit trigger address + succ. + predecessor bits
    auto entry_size = 34 + precSize + succSize;
    auto total_size = (maxCompactorEntries
                    + p.stream_address_buffer_entries
                    + p.history_buffer_size) * entry_size
                    + p.index_entries * (34+15);

    warn("PIF Prefetcher with: Index size: %d entries => %dkB"
        "Hist: %d entries => %dk ==> total %dk",
        p.index_entries, p.index_entries*(34+15)/8/1024,
        p.history_buffer_size, p.history_buffer_size * entry_size/8/1024,
        total_size /8/1024);

    for (int i=0; i < maxCompactorEntries; i++) {
        temporalCompactor.push_back(CompactorEntry());
    }

}


PIF::CompactorEntry::CompactorEntry(Addr addr,
    unsigned int prec_size, unsigned int succ_size)
{
    trigger = addr;
    prec.resize(prec_size, false);
    succ.resize(succ_size, false);
    bits.reset();
    bits.set(0);
}


bool
PIF::CompactorEntry::isSupersetOf(CompactorEntry& other) const
{
    if (trigger != other.trigger) {
        return false;
    }
    for (int i = prec.size()-1; i >= 0; i--) {
        // If the other record has a bit set bit this one not
        // its not a subset
        if (other.prec[i] && !prec[i]) {
            return false;
        }
    }
    for (int i = 0; i < succ.size(); i++) {
        // Address from the succeding blocks to issue a prefetch
        if (other.succ[i] && !succ[i]) {
            return false;
        }
    }
    return true;
}

unsigned
PIF::CompactorEntry::numBlocks() const
{
    // TODO
    unsigned n = 0;
    for (auto i : prec)
        n += i ? 1 : 0;
    for (auto i : succ)
        n += i ? 1 : 0;
    return n;
}


bool
PIF::CompactorEntry::inSameSpatialRegion(Addr pc,
        unsigned int log_blk_size, bool update)
{

    if (trigger <= pc && pc < (trigger + (_regionSize << log_blk_size))) {
        if (update)
            bits.set(getIndex(pc, log_blk_size));
        return true;
    }
    return false;
};

int
PIF::CompactorEntry::getIndex(uint64_t addr, unsigned int log_blk_size) const
{
    // int index = (anAddress - theRegionBase)/theBlockSize
    // + (regionSize/2-spatialOffset);
    int index = (addr - trigger) >> log_blk_size;

    assert(0 <= index && index < _max_bits);
    return index;
};

bool
PIF::CompactorEntry::hasAddress(Addr target,
                                          unsigned int log_blk_size) const
{
    if (trigger <= target &&
        target < (trigger + (_regionSize << log_blk_size))) {
        int index = getIndex(target, log_blk_size);
        return bits[index];
    }
    return false;
};


void
PIF::CompactorEntry::getPredictedAddresses(unsigned int log_blk_size,
                                        std::list<Addr> &addresses) const
{
    // Calculate the addresses of the instruction blocks that are encoded
    // by the bit vector and issue prefetch requests for these addresses.
    // Predictions are made by traversing the bit vector from left to right
    // as this typically predicts the accesses in the order they will be
    // issued in the core.
    const Addr trigger_blk = trigger >> log_blk_size;

    // The trigger block itself.
    addresses.push_back(trigger_blk);

    for (int i = prec.size()-1; i >= 0; i--) {
        // Address from the preceding blocks to issue a prefetch
        if (prec[i]) {
            const Addr prec_addr = (trigger_blk - (i+1)) << log_blk_size;
            addresses.push_back(prec_addr);
        }
    }
    for (int i = 0; i < succ.size(); i++) {
        // Address from the succeding blocks to issue a prefetch
        if (succ[i]) {
            const Addr succ_addr = (trigger_blk + (i+1)) << log_blk_size;
            addresses.push_back(succ_addr);
        }
    }
}


void
PIF::record(const Addr pc)
{

    statsPIF.pifRecordPC++;
    Addr inst_blk = blockAddress(pc);

    DPRINTF(HWPrefetch, "PIF (rec): Notify PC: %#x Inst. Blk: %#x\n",
            pc, inst_blk);


    bool evict = true;
    int hit_idx = 0;

    for (auto entry = temporalCompactor.begin();
            entry != temporalCompactor.end(); entry++, hit_idx++) {

        if (entry->inSameSpatialRegion(pc, lBlkSize, true)) {
        // If the PC of the instruction retired is in the same spatial region
        // than the last trigger address, update the bit vectors based on the
        // distance between them
            DPRINTF(HWPrefetch, "PIF (rec): PC:%#x, Blk: %#x: "
                        "Hit in %i -> E:(%s), bi:%i\n",
                        pc, inst_blk, hit_idx, entry->print(),
                        entry->getIndex(pc,lBlkSize));


            if (hit_idx == 0) {
                statsPIF.pifNHitSpatial++;
                DPRINTF(HWPrefetch,
                        "PIF (rec): Inst. Blk: %#x Hit spatial:\n",inst_blk);
            } else {
        // If the PC of the instruction retired is outside the latest spatial
        // region, check if it matches in any of the regions in the temporal
        // compactor and update it to the MRU position
                statsPIF.pifNHitTemporal++;
                DPRINTF(HWPrefetch, "PIF (rec): Inst. Blk: %#x "
                        "Hit temporal[%i]\n", inst_blk, hit_idx);

                if (compactor_lru) {
                    temporalCompactor.splice(temporalCompactor.begin(),
                                            temporalCompactor, entry);
                }


            }

            evict = false;
            break;
        }
    }


    if (evict) {

        // No hit in the temporal compactor. Create a new
        // record with the actual PC. Reset the spatial compactor
        // updating the trigger address and resetting the vector bits
        Addr trigger = useBlkAddr ? inst_blk : pc;
        // temporalCompactor.push_front(
        //                         CompactorEntry(trigger, precSize, succSize));

        temporalCompactor.push_back(
                                CompactorEntry(trigger, precSize, succSize));
        // In case the temporal compactor exceeded its limit
        // the LRU entry will now be send to the history buffer.
        auto record = temporalCompactor.front();
        temporalCompactor.pop_front();



        // Insert into the history buffer and update
        // the 'iterator' table to point to the new entry
        historyBuffer.push_back(record);
        statsPIF.pifNHistWrites++;
        DPRINTF(HWPrefetch, "PIF (rec): %#x not found in compressor. "
                            "Write LRU record : %s\n",
                            inst_blk, record.print());

        IndexEntry *idx_entry =
            index.findEntry(record.trigger, false);


        if (idx_entry != nullptr) {
            index.accessEntry(idx_entry);
            DPRINTF(HWPrefetch, "PIF (rec): Trigger exists in index: "
                            "%s.\n",idx_entry->historyIt->print());
            statsPIF.pifNIndexOverride++;
        } else {
            idx_entry = index.findVictim(record.trigger);
            assert(idx_entry != nullptr);
            index.insertEntry(record.trigger, false,
                                idx_entry);
            statsPIF.pifNIndexInsert++;
            DPRINTF(HWPrefetch, "PIF (rec): Insert new trigger in "
                    "index: %#x\n", record.trigger);
        }
        idx_entry->historyIt =
            historyBuffer.getIterator(historyBuffer.tail());
        DPRINTF(HWPrefetch, "PIF (rec): Set Index %#x to %#x: (%s)\n",
                                record.trigger, historyBuffer.tail(),
                                idx_entry->historyIt->print());
    }
}


void
PIF::predict(const Addr pc)
{
    Addr inst_blk = blockAddress(pc);
    // Addr trigger = useBlkAddr ? inst_blk : pc;

    // First check if the access has been prefetched, this is done by
    // comparing the access against the active Stream Address Buffers

    int hit_idx = 0;

    // Find the trigger address in the stream address buffer.
    // The two parameters SAB size and lookahead distance are used to have s
    // small window that allows a little bit of stream divergence.
    // Whenever a prediction is made the SAB is searched from the beginning
    // until we found the entry that holds the specific block address. If
    // the hit possition is larger then 'n', hence less that the lookahead
    // distance to end of the SAB the stream is advanced.
    // If its less or equal to possition 'n' the stream is not advance.
    //
    // SAB: [0 ...... n .............. m]
    //                ^
    //                | <- lookahead -> |
    //
    //       | < ------ SAB size ---- > |


    auto sabEntry = streamAddressBuffer.begin();
    for (; sabEntry != streamAddressBuffer.end(); sabEntry++, hit_idx++) {

        if ((*sabEntry)->hasAddress(pc, lBlkSize)) {
            // We are done
            break;
        }
    }

    HistoryPtr hist_tail = historyBuffer.end();
    /** if the index */
    if (sabEntry != streamAddressBuffer.end()) {
        // We hit in the SAB
        statsPIF.pifQSABHits++;
        DPRINTF(HWPrefetch, "PIF (pred): Inst. Blk: %#x Hit in SAB[%i]\n",
                            inst_blk, hit_idx);


        // Get the last element in the steam address buffer.
        // Its a pointer to the history that was read last time.
        // Advance it by one to get the next history element to read.
        hist_tail = streamAddressBuffer.back();
        hist_tail++;

        // If we hit within the lookahead window andvance it by poping
        // the first elements.
        // The refilling will happen later.
        while (hit_idx > (sabSize - (int)lookahead)) {


            DPRINTF(HWPrefetch, "PIF (pred): hit idx (%i) > "
                                "(sabSize - lookahead) (%i)\n",
                                    hit_idx, lookahead);

            // If we see an access further ahead in the stream advance
            // to the new prefetches for this drop the oldest entries.
            // Refill will be happen later
            streamAddressBuffer.pop_front();
            hit_idx--;
        }

    } else {

    // Miss in the Stream address buffer. We need to reset.
    // Check if a valid entry in the 'index' table is found and allocate a new
    // active prediction stream
    Addr trigger = useBlkAddr ? inst_blk : pc;
    IndexEntry *idx_entry = index.findEntry(trigger, /* unused */ false);
    statsPIF.pifQIndexReset++;


    if (idx_entry != nullptr) {
        statsPIF.pifQIndexResetHit++;
        DPRINTF(HWPrefetch, "PIF (pred): Reindex (hit): T:%#x, E:%s\n",
                    trigger, idx_entry->historyIt->print());
        index.accessEntry(idx_entry);
        // Trigger address from the 'index' table and index to the history
        // buffer
        hist_tail = idx_entry->historyIt;
        streamAddressBuffer.clear();

    } else {
        // Miss in the index. We dont know what to prefetch
        statsPIF.pifQIndexResetMiss++;
        DPRINTF(HWPrefetch, "PIF (pred): Reindex (miss): %#x \n", inst_blk);
    }

    }


    // If we poped an entry from the SAB or resert the index
    // we need to refill it by reading enties from the history buffer
    // and issue the corresponding prefetch requests

    while (streamAddressBuffer.size() < sabSize)  {

        // Check for history overflows
        if (hist_tail == historyBuffer.end()) {
            break;
        }
        DPRINTF(HWPrefetch, "PIF (pred): Refill SAB[%i] with: %s\n",
                    streamAddressBuffer.size()+1, hist_tail->print());
        streamAddressBuffer.push_back(hist_tail);
        hist_tail->getPredictedAddresses(lBlkSize, prefetchCandiates);
        hist_tail++;
    }
}






void
PIF::calculatePrefetch(const PrefetchInfo &pfi,
    std::vector<AddrPriority> &addresses)
{
    statsPIF.pifQueries++;
    if (!pfi.hasPC()) {
        return;
    }

    if (hasBeenPrefetched(pfi.getAddr(), pfi.isSecure())) {
        statsPIF.pifQHasBeenPref++;
        return;
    }

    const Addr pc = pfi.getPC();

    /** Make prediction except we do predictions only on retired PC*/
    if (!predictOnRetiredPC) {
        predict(pc);
    }

    // Get the predicted addresses if some exist.
    for (Addr a : prefetchCandiates) {
        addresses.push_back(AddrPriority(a, 0));
    }
    prefetchCandiates.clear();
}



void
PIF::notifyRetiredInst(const Addr pc)
{
    statsPIF.pifNRetInst++;
    /** Make prediction if configured to do so */
    if (predictOnRetiredPC) {
        predict(pc);
    }

    /** Record this PC*/
    record(pc);
}


// void
// PIF::PrefetchListenerPC::notify(const Addr& pc)
// {
//     parent.notifyRetiredInst(pc);
// }

// void
// PIF::addEventProbeRetiredInsts(SimObject *obj, const char *name)
// {
//     warn("Register HWP event probe %s for obj: %s", name, obj->name());
//     ProbeManager *pm(obj->getProbeManager());
//     listenersPC.push_back(new PrefetchListenerPC(*this, pm, name));
// }

void
PIF::regProbeListeners()
{
    Base::regProbeListeners();

    if (cpu == nullptr) {
        warn("No CPU to listen from registered\n");
        return;
    }
    typedef ProbeListenerArgFn<Addr> RetPCListener;
    listeners.push_back(
            new RetPCListener(cpu->getProbeManager(), "RetiredInstsPC",
                [this](const Addr &pc)
                    { notifyRetiredInst(pc); }));
}

PIF::PIFStats::PIFStats(statistics::Group *parent)
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

    ADD_STAT(pifNRetInst, statistics::units::Count::get(),
            "number of evictions from the index"),
    ADD_STAT(pifNIndexInsert, statistics::units::Count::get(),
            "number of evictions from the index"),
    ADD_STAT(pifNIndexOverride, statistics::units::Count::get(),
            "number of overrides of an previous entry."),
    ADD_STAT(pifNHistWrites, statistics::units::Count::get(),
            "number of writes to the history buffer"),
    ADD_STAT(pifNHitSpatial, statistics::units::Count::get(),
            "number of prefetches hit in temp. compactor"),
    ADD_STAT(pifNHitTemporal, statistics::units::Count::get(),
            "number of prefetches hit in temp. compactor")
{
}


} // namespace prefetch
} // namespace gem5
