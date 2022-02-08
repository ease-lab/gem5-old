/**
 * Copyright (c) 2019 Metempsy Technology Consulting
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
      historyBuffer(p.history_buffer_size),
      index(p.index_assoc, p.index_entries, p.index_indexing_policy,
            p.index_replacement_policy),
      streamAddressBuffer(p.stream_address_buffer_entries),
      listenersPC(),
      statsPIF(this)
{
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

#ifdef ADDR_PRINT
    retInstFile  = simout.create(name() + "." + "ret_inst.bin", true);
    predInstFile = simout.create(name() + "." + "pred_inst.bin", true);
    histFile = simout.create(name() + "." + "hist.txt", false);
    sabFile = simout.create(name() + "." + "sab.txt", false);
#endif
}

PIF::CompactorEntry::CompactorEntry(Addr addr,
    unsigned int prec_size, unsigned int succ_size)
{
    trigger = addr;
    prec.resize(prec_size, false);
    succ.resize(succ_size, false);
}

Addr
PIF::CompactorEntry::distanceFromTrigger(Addr target,
        unsigned int log_blk_size) const
{
    const Addr target_blk = target >> log_blk_size;
    const Addr trigger_blk = trigger >> log_blk_size;

    return target_blk > trigger_blk ?
              target_blk - trigger_blk : trigger_blk - target_blk;
}

bool
PIF::CompactorEntry::inSameSpatialRegion(Addr pc,
        unsigned int log_blk_size, bool update)
{
    Addr blk_distance = distanceFromTrigger(pc, log_blk_size);
    // if (pc == trigger) {
    //     return true;
    // }
    bool hit = (pc > trigger) ?
        (succ.size() > blk_distance) : (prec.size() > blk_distance);
    if (hit && update) {
        if (pc > trigger) {
            succ[blk_distance] = true;
        } else if (pc < trigger) {
            prec[blk_distance] = true;
        }
    }
    return hit;
}

bool
PIF::CompactorEntry::hasAddress(Addr target,
                                          unsigned int log_blk_size) const
{
    Addr blk_distance = distanceFromTrigger(target, log_blk_size);
    bool hit = false;
    if (target > trigger) {
        hit = blk_distance < succ.size() && succ[blk_distance];
    } else if (target < trigger) {
        hit = blk_distance < prec.size() && prec[blk_distance];
    } else {
        hit = true;
    }
    return hit;
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

void
PIF::CompactorEntry::getPredictedAddresses(unsigned int log_blk_size,
    std::vector<AddrPriority> &addresses) const
{
    // Calculate the addresses of the instruction blocks that are encoded
    // by the bit vector and issue prefetch requests for these addresses.
    // Predictions are made by traversing the bit vector from left to right
    // as this typically predicts the accesses in the order they will be
    // issued in the core.
    const Addr trigger_blk = trigger >> log_blk_size;

    // The trigger block itself.
    addresses.push_back(AddrPriority(trigger_blk, 0));

    for (int i = prec.size()-1; i >= 0; i--) {
        // Address from the preceding blocks to issue a prefetch
        if (prec[i]) {
            const Addr prec_addr = (trigger_blk - (i+1)) << log_blk_size;
            addresses.push_back(AddrPriority(prec_addr, 0));
        }
    }
    for (int i = 0; i < succ.size(); i++) {
        // Address from the succeding blocks to issue a prefetch
        if (succ[i]) {
            const Addr succ_addr = (trigger_blk + (i+1)) << log_blk_size;
            addresses.push_back(AddrPriority(succ_addr, 0));
        }
    }
}

void
PIF::notifyRetiredInst(const Addr pc)
{
#ifdef ADDR_PRINT
    if (retInstFile->stream()->good()) {
        auto tick = curTick();
        retInstFile->stream()->write(reinterpret_cast<const char*>(&tick),
                                            sizeof(gem5::Tick));
        retInstFile->stream()->write(reinterpret_cast<const char*>(&pc),
                                            sizeof(Addr));
        retInstFile->stream()->flush();
    }
#endif

    statsPIF.pifNRetInst++;
    const Addr inst_blk = blockAddress(pc);

    DPRINTF(HWPrefetch, "PIF (N): Notify PC: %#x Inst. Blk: %#x\n",
            pc, inst_blk);

    // First access to the prefetcher
    if (spatialCompactor.trigger == MaxAddr) {
        spatialCompactor = CompactorEntry(pc, precSize, succSize);
        // temporalCompactor.push_back(spatialCompactor);
    } else {
        // If the PC of the instruction retired is in the same spatial region
        // than the last trigger address, update the bit vectors based on the
        // distance between them
        if (spatialCompactor.inSameSpatialRegion(pc, lBlkSize, true)) {
        // If the PC of the instruction retired is outside the latest spatial
        // region, check if it matches in any of the regions in the temporal
        // compactor and update it to the MRU position
        statsPIF.pifNHitSpatial++;
        DPRINTF(HWPrefetch, "PIF (N): Inst. Blk: %#x Hit spatial\n",inst_blk);
        } else {

            // The PC is not in the spatial compactor
            // A new record need to be created. Before doing so
            // the actual one needs to be inserted in the temporal compactor.
            //
            // Check if for the actual record a superset can be found.

            for (auto it = temporalCompactor.begin();
                    it != temporalCompactor.end(); it++)
            {
                DPRINTF(HWPrefetch, "PIF (N): Lookup record: %s in temp. "
                        "compactor: %s\n", spatialCompactor.print(),
                                            it->print());
                if (it->isSupersetOf(spatialCompactor)) {

                    // If the compactor entry is found in the temporal
                    // compactor the actual one can be discarded.
                    // The old entry will be promoted to the MRU possition
                    // in the temporal compactor
                    spatialCompactor = (*it);
                    temporalCompactor.erase(it);
                    statsPIF.pifNHitTemporal++;
                    DPRINTF(HWPrefetch, "PIF (N): Inst. Blk: %#x "
                            "Hit temporal\n", inst_blk);
                    break;
                }
            }
            // Enqueue the actual spatial compactor
            temporalCompactor.push_back(spatialCompactor);

            // Now the spacial compactor is free and we can create the new
            // record with the actual PC. Reset the spatial compactor
            // updating the trigger address and resetting the vector bits
            spatialCompactor = CompactorEntry(pc, precSize, succSize);

            if (temporalCompactor.size() > maxCompactorEntries) {
                // In case the temporal compactor exceeded its limit
                // the LRU entry will now be send to the history buffer.
                auto record = temporalCompactor.front();
                temporalCompactor.pop_front();

                // Insert into the history buffer and update
                // the 'iterator' table to point to the new entry
                historyBuffer.push_back(record);
                statsPIF.pifNHistWrites++;
                DPRINTF(HWPrefetch, "PIF (N): %#x not found in compressor. "
                                    "Write LRU record with trigger %#x \n",
                                        inst_blk, record.trigger);

                IndexEntry *idx_entry =
                    index.findEntry(record.trigger, false);

#ifdef ADDR_PRINT
            if (histFile->stream()->good()) {
                auto tick = curTick();
                std::string hit = (idx_entry != nullptr)
                                ? " | i\n" : " | n\n";
                std::string s(record.print() + hit);
                histFile->stream()->write(s.c_str(), s.size());
                histFile->stream()->flush();
            }
#endif


                if (idx_entry != nullptr) {
                    index.accessEntry(idx_entry);
                    DPRINTF(HWPrefetch, "PIF (N): Trigger exists in index: "
                                    "%s.\n",idx_entry->historyIt->print());
                } else {
                    idx_entry = index.findVictim(record.trigger);
                    assert(idx_entry != nullptr);
                    index.insertEntry(record.trigger, false,
                                      idx_entry);
                    statsPIF.pifNIndexInsert++;
                    DPRINTF(HWPrefetch, "PIF (N): Insert new trigger in "
                            "index: %#x\n", record.trigger);
                }
                idx_entry->historyIt =
                    historyBuffer.getIterator(historyBuffer.tail());
                DPRINTF(HWPrefetch, "PIF (N): Set Index %#x to %x\n",
                                        record.trigger, historyBuffer.tail());

            }
        }
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
    const Addr inst_blk = pc; //blockAddress(pc);

    DPRINTF(HWPrefetch, "PIF (Q): Query PC: %#x Inst. Blk: %#x %s in cache\n",
            pc, inst_blk, (pfi.isCacheMiss()) ? "miss" : "hit");

    // First check if the access has been prefetched, this is done by
    // comparing the access against the active Stream Address Buffers
    int hit_idx = 0;
    for (auto &sabEntry : streamAddressBuffer) {
        DPRINTF(HWPrefetch, "PIF (Q): Lookup Blk: %#x in SAB: %#x Blk: %#x\n",
                inst_blk, sabEntry->trigger, blockAddress(sabEntry->trigger));
        if (sabEntry->hasAddress(pc, lBlkSize)) {
            // We are done
            break;
        }
        hit_idx++;
    }
    if (hit_idx < streamAddressBuffer.size()) {
        // We hit in the SAB
        statsPIF.pifQSABHits++;
        DPRINTF(HWPrefetch, "PIF (Q): Inst. Blk: %#x Hit in SAB\n", inst_blk);

        if (hit_idx > 2) {
            // If we see an access further ahead in the stream advance
            // to the new prefetches for this drop the oldes entry.
            // Refill will be happen later
            streamAddressBuffer.pop_front();
        }
    } else {

    DPRINTF(HWPrefetch, "PIF (Q): Inst. Blk: %#x Reindex\n", inst_blk);
    // Miss in the Stream address buffer. We need to reset.
    // Check if a valid entry in the 'index' table is found and allocate a new
    // active prediction stream
    IndexEntry *idx_entry = index.findEntry(pc, /* unused */ false);
    statsPIF.pifQIndexReset++;
    streamAddressBuffer.flush();
    // Clear all enqueued prefetches.
    pfq.clear();

    if (idx_entry != nullptr) {
        statsPIF.pifQIndexResetHit++;
        DPRINTF(HWPrefetch, "PIF (Q): Index hit for PC: %#x Blk: %#x\n",
                pc, inst_blk);
        index.accessEntry(idx_entry);
        // Trigger address from the 'index' table and index to the history
        // buffer
        auto entry = idx_entry->historyIt;

        // Track the block in the Stream Address Buffer
        DPRINTF(HWPrefetch, "PIF (Q): Refill SAB with: %s\n",
                entry->print());
        streamAddressBuffer.push_back(entry);
        entry->getPredictedAddresses(lBlkSize, addresses);

#ifdef ADDR_PRINT
        if (sabFile->stream()->good()) {
            auto tick = curTick();
            std::string hit = " | n\n";
            std::string s(entry->print() + hit);
            sabFile->stream()->write(s.c_str(), s.size());
            sabFile->stream()->flush();
        }
#endif

    } else {
        // Miss in the index. We dont know what to prefetch
        statsPIF.pifQIndexResetMiss++;
        DPRINTF(HWPrefetch, "PIF (Q): Index miss for PC: %#x Blk: %#x\n",
                pc, inst_blk);
    }

    }

    // If we poped an entry from the SAB or reserted it
    // we need to refill it by reading enties from the history buffer
    // and issue the corresponding prefetch requests
    if (streamAddressBuffer.size() > 0) {
        HistoryBuffer::iterator hist_tail = streamAddressBuffer.back();
            // historyBuffer.getIterator();
        while (!streamAddressBuffer.full())  {
        // if (hist_tail == historyBuffer.getIterator(historyBuffer.tail())) {
        //     // History is at the end
        //     break;
        // }
            hist_tail++;
            DPRINTF(HWPrefetch, "PIF (Q): Refill SAB with: %s\n",
                hist_tail->print());
            streamAddressBuffer.push_back(hist_tail);
            hist_tail->getPredictedAddresses(lBlkSize, addresses);

#ifdef ADDR_PRINT
            if (sabFile->stream()->good()) {
                auto tick = curTick();
                std::string hit = " | f\n";
                std::string s(hist_tail->print() + hit);
                sabFile->stream()->write(s.c_str(), s.size());
                sabFile->stream()->flush();
            }
#endif
        }
    }

#ifdef ADDR_PRINT
    if (predInstFile->stream()->good()) {
        auto tick = curTick();
        for (auto addr : addresses) {
            predInstFile->stream()->write(
                    reinterpret_cast<const char*>(&tick), sizeof(gem5::Tick));
            predInstFile->stream()->write(
                    reinterpret_cast<const char*>(&addr), sizeof(Addr));
            predInstFile->stream()->flush();
        }
    }
#endif

}

void
PIF::PrefetchListenerPC::notify(const Addr& pc)
{
    parent.notifyRetiredInst(pc);
}

void
PIF::addEventProbeRetiredInsts(SimObject *obj, const char *name)
{
    warn("Register HWP event probe %s for obj: %s", name, obj->name());
    ProbeManager *pm(obj->getProbeManager());
    listenersPC.push_back(new PrefetchListenerPC(*this, pm, name));
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
