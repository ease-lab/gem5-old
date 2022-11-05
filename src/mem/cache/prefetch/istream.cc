/*
 * Copyright (c) 2021 EASE Group, The University of Edinburgh
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
 * Describes a IStream prefetcher based on template policies.
 */


/********************************************************
 *         I-Stream prefetcher aka. Jukebox
 ********************************************************
 * This prefetcher is specially designed to limit the
 * impact of cold hardware structures we investigate
 * for serverless functions comming from the fact the
 * 100s to thousands of functions are scheduled on the
 * same CPU core and there are a lot of context switches
 * between the different functions.
 *
 * The Instruction stream prefetcher exists of two part:
 *
 * - Record:
 *    This component track instruction accesses and store
 *    them in compressed form in a trace.
 *    Note this is a prefetcher designed specifically for
 *    optimizing start ups and cold starts that means as
 *    soon as the trace cannot hold more entries the
 *    prefetcher will stop recording.
 *
 * - Replay:
 *    This component is independent of the recording.
 *    It will read a prevously recorded trace, create
 *    prefetches to the prevously recorded addresse in order
 *    to basically restore the cache lines we loose while
 *    the function was descheduled.
 */

#include "mem/cache/prefetch/istream.hh"

#include <algorithm>

#include "debug/HWPrefetch.hh"
#include "mem/cache/base.hh"



namespace gem5 {

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch {

// namespace StreamPref {



/*****************************************************************
 * Record Buffer functionality
 */

Addr
RecordBuffer::regionAddress(Addr a) { return a & ~((Addr)regionSize - 1); }

unsigned
RecordBuffer::regionIndex(Addr a) { return (a % regionSize) >> lBlkSize; }

std::pair<Addr, unsigned>
RecordBuffer::regionAddrIdx(Addr addr) {
    return std::make_pair(regionAddress(addr), regionIndex(addr));
}


void RecordBuffer::access(gem5::Addr addr, uint64_t history_tag) {
    auto region = regionAddrIdx(addr);

    // Lookup if the buffer contains already an entry for this region
    // auto it = std::find_if(_buffer.begin(), _buffer.end(),
    //   [region](BufferEntry e) -> bool {
    //     return region.first == e._addr;
    //   });
    int hit_idx = 0;
    for (; hit_idx < _buffer.size(); hit_idx++) {
        if (_buffer[hit_idx]->_addr == region.first) {
            break;
        }
    }

    if (hit_idx < _buffer.size()) {
        stats.hits++;
        stats.bufferHitDistHist.sample(hit_idx);
        // DPRINTF(HWPrefetch, "[%#x] hit: Region (%#x:%u)\n",
        //   addr, _buffer[hit_idx]->_addr, region.second);
    } else {
        stats.misses++;
        DPRINTF(HWPrefetch,
                "[%#x] miss. "
                "Add new entry to the record buffer\n",
                addr);

        // Create new entry and push it into the fifo buffer
        add(region.first);
        // Add a history tag when the entry was created.
        _buffer[hit_idx]->history = parent->curCycle() - last_timestamp;
        last_timestamp = parent->curCycle();
        hit_idx = 0;
    }

    if (!_buffer[hit_idx]->check(region.second)) {
        parent->pfRecorded++;
    }
    // Mark the entry block in the region as used
    _buffer[hit_idx]->touch(region.second);

    // parent->calcStackDistance(region.first);
}


// void RecordBuffer::access(gem5::Addr addr, uint64_t history_tag) {
//     auto region = regionAddrIdx(addr);

//     BufferPtr buffer = getBuffer(region.first);

//     // Lookup if the buffer contains already an entry for this region
//     // auto it = std::find_if(_buffer.begin(), _buffer.end(),
//     //   [region](BufferEntry e) -> bool {
//     //     return region.first == e._addr;
//     //   });
//     int hit_idx = 0;
//     for (; hit_idx < buffer->size(); hit_idx++) {
//         if (buffer->at(hit_idx)->_addr == region.first) {
//             break;
//         }
//     }

//     // BufferEntryPtr * entry = buffer.findEntry(region.first, false);





//     if (hit_idx < buffer->size()) {
//         stats.hits++;
//         stats.bufferHitDistHist.sample(hit_idx);
//         // DPRINTF(HWPrefetch, "[%#x] hit: Region (%#x:%u)\n",
//         //   addr, _buffer[hit_idx]->_addr, region.second);
//     } else {
//         stats.misses++;
//         DPRINTF(HWPrefetch,
//                 "[%#x] miss. Add entry to record buffer[%i]\n",
//                 addr, getIdx(region.first));

//         // Create new entry and push it into the fifo buffer
//         // add(region.first);
//         buffer->push_front(std::make_shared<BufferEntry>());
//         // Add a history tag when the entry was created.
//         buffer->at(hit_idx)->_addr = region.first;
//         buffer->at(hit_idx)->_inst = true;
//         buffer->at(hit_idx)->history = parent->curCycle() - last_timestamp;
//         last_timestamp = parent->curCycle();
//         hit_idx = 0;
//     }

//     if (!buffer->at(hit_idx)->check(region.second)) {
//         parent->pfRecorded++;
//     }
//     // Mark the entry block in the region as used
//     buffer->at(hit_idx)->touch(region.second);

//     // parent->calcStackDistance(region.first);
// }


void RecordBuffer::flush(bool all) {
    // Resize the buffer by writting the last
    // entries into the trace file.
    // Write everything when the all flag is set.
    while (_buffer.size() > bufferEntries || (_buffer.size() && all)) {
        auto wrEntry = _buffer.back();
        auto n_pref = wrEntry->bitsSet();
        float use = usefullness(wrEntry);
        stats.totalNumPrefetches += n_pref;

        // TODO parameter
        const float threshold = 0;
        if (use > threshold) {
            stats.entryWrites++;
            DPRINTF(HWPrefetch,
                    "Rec. buffer entry %#x:[%x]: write to memory."
                    "Contains: %i prefetches -> Usefullness: %.3f\n",
                    wrEntry->_addr, wrEntry->_mask, n_pref, use);

            if (!parent->memInterface->writeRecord(wrEntry)) {
                stats.entryOverflows++;
            }
            // parent->recordStream->write(wrEntry);
        } else {
            stats.entryDrops++;
            DPRINTF(HWPrefetch,
                    "Drop buffer entry [%#x:%x] drop. "
                    "Usefullness: %.3f\n",
                    wrEntry->_addr, wrEntry->_mask, use);
        }
        _buffer.pop_back();
    }
}

// void RecordBuffer::flush(bool all) {
//     // Resize the buffer by writting the last
//     // entries into the trace file.
//     // Write everything when the all flag is set.
//     for (uint i = 0; i < assoc; i++) {
//         BufferPtr buffer = getBuffer(i);

//         while (buffer->size() > buf_entries || (buffer->size() && all)) {
//             auto wrEntry = buffer->back();
//             auto n_pref = wrEntry->bitsSet();
//             float use = usefullness(wrEntry);
//             stats.totalNumPrefetches += n_pref;

//             // TODO parameter
//             const float threshold = 0;
//             if (use > threshold) {
//                 stats.entryWrites++;
//                 DPRINTF(HWPrefetch,
//                         "Rec. buffer entry %#x:[%x]: write to memory."
//                         "Contains: %i prefetches -> Usefullness: %.3f\n",
//                         wrEntry->_addr, wrEntry->_mask, n_pref, use);

//                 if (!parent->memInterface->writeRecord(wrEntry)) {
//                     stats.entryOverflows++;
//                 }
//                 // parent->recordStream->write(wrEntry);
//             } else {
//                 stats.entryDrops++;
//                 DPRINTF(HWPrefetch,
//                         "Drop buffer entry [%#x:%x] drop. "
//                         "Usefullness: %.3f\n",
//                         wrEntry->_addr, wrEntry->_mask, use);
//             }
//             buffer->pop_back();
//         }
//     }
// }

/*****************************************************************
 * Replay Buffer functionality
 */

// /**
//  * Check if there is an entry for this address in the replay buffer.
//  * If so the all addresses will be generated for prefetching.
//  * Finally, the entry can be dropped.
//  */

// void ReplayBuffer::probe(gem5::Addr addr) {
//     auto region = parent->regionAddrIdx(addr);

//     // Check if the replay buffer contains the entry for this region
//     int hit_idx = 0;
//     auto it = _buffer.begin();
//     for (; it != _buffer.end(); it++, hit_idx++) {
//         if ((*it)->_addr == region.first && (*it)->check(region.second)) {
//             break;
//         }
//     }

//     if (it == _buffer.end()) {
//         return;
//     }

//     stats.notifyHits++;
//     // stats.bufferHitDistHist.sample(hit_idx);
//     // DPRINTF(HWPrefetch, "hit: Region (%#x:%u). Move to front.\n",
//     //                   (*it)->_addr, region.second);

//     // In the case we found an entry within the replay buffer we are
//     // already to late to for this to be prefetched.
//     // However, then we will try to prefetch the other once as soon as
//     // possible. Therefore, we put this entry to the head of the replay
//     // buffer.
//     _buffer.splice(_buffer.begin(), _buffer, it);
//     // _buffer[hit_idx]->genAllAddr(addresses);

//     // // And drop the entry
//     // _buffer.erase(_buffer.begin() + hit_idx);
// }

std::vector<Addr> ReplayBuffer::generateNAddresses(int n) {
    std::vector<Addr> addresses;
    int n_left = n;

    // Starting from the beginning of the list generate
    // up to n addresses.
    for (auto it = _buffer.begin(); it != _buffer.end();) {
        n_left -= genNAddr(*it, n_left, addresses);
        if (n_left == 0) {
            break;
        }
        // If this
        if ((*it)->empty()) {
            it++;
            _buffer.pop_front();
        }
    }
    return addresses;
}

bool ReplayBuffer::refill() {
    // In case we are already at the end of the stream we
    // cannot request more entries.
    if (parent->memInterface->replayDone()) {
        return false;
    }
    // DPRINTF(HWPrefetch, "Refill: bsz: %i, pending: %i, entries: %i\n",
    //                         _buffer.size(),pendingFills, bufferEntries);
    while ((_buffer.size() + pendingFills) < bufferEntries) {
        if (!parent->memInterface->readRecord()) {
            DPRINTF(HWPrefetch, "Reached end of trace.\n");
            return false;
        }
        pendingFills++;
    }
    return true;
}

void ReplayBuffer::fill(BufferEntryPtr entry) {
    _buffer.push_back(entry);
    pendingFills--;
    DPRINTF(HWPrefetch, "Fill: [%s]. size buf.:%i pend. fills %i\n",
            entry->print(), _buffer.size(), pendingFills);
    stats.entryReads++;
}

/*****************************************************************
 * Memory Interface
 * Helper that contains all functionalities to interact with the
 * records in memory
 */
MemoryInterface::MemoryInterface(IStream& _parent,
                                          AddrRange record_addr_range,
                                          AddrRange replay_addr_range,
                                          unsigned trace_size,
                                          PortProxy& _memProxy,
                                          std::string _name)
    : _name(_name),
      parent(_parent),
      memProxy(&_memProxy),
      record_addr_range(record_addr_range),
      replay_addr_range(replay_addr_range),
      recordIdx(0),
      totalRecordEntries(0),
      replayIdx(0),
      totalReplayEntries(0),
      entry_size(0),
      blk_size(_parent.blkSize),
      region_size(_parent.regionSize),
      history_bits(_parent.historyBits),
      trace_size(trace_size),
      stats(&_parent, _name)
{

    uint lRegionSize = floorLog2(region_size);
    uint blksPerRegion = region_size / blk_size;

    // Calculate the real entry size not the one we have for modeling.
    int bits_region_ptr = 48 - lRegionSize;
    // entrySize_n_addr = ceil(float(bits_region_ptr) / 8);
    // entrySize_n_mask = ceil(float(blksPerRegion) / 8);
    // entrySize_n_hist = ceil(float(historyBits) / 8);

    entrySize_n_addr = ceil(float(bits_region_ptr) / 32) * 4;
    entrySize_n_mask = ceil(float(blksPerRegion) / 32) * 4;
    entrySize_n_hist = ceil(float(history_bits) / 32) * 4;

    entry_size = entrySize_n_addr + entrySize_n_mask + entrySize_n_hist;
    effectiveEntrySize = bits_region_ptr + blksPerRegion + history_bits + 1;
    // warn("Effective Entry size: %i, (%i + %i + %i)")

    // Calculate how many entries fit into one CL.
    entries_per_cl = blk_size / entry_size;

    DPRINTF(HWPrefetch,
            "Cfg: region size: %i, blk size %i => "
            "%i blks/region => Entry size: %ib (%ib + %ib + %ib) "
            "Model: %iB (%iB + %iB + %iB)."
            " %i entries per CL\n",
            region_size, blk_size, blksPerRegion, effectiveEntrySize,
            bits_region_ptr, blksPerRegion, history_bits + 1, entry_size,
            entrySize_n_addr, entrySize_n_mask, entrySize_n_hist,
            entries_per_cl);


    // Before enabling this feature we need to ensure that everything works as
    // intended.
    // assert(maxOutstandingReqs == 0);
    int max_record_entries = record_addr_range.size() / entry_size;
    int max_replay_entries = replay_addr_range.size() / entry_size;
    DPRINTF(HWPrefetch,
            "Record mem cfg: range: %s"
            " entry size: %iB => capacity: %ikB = %i entries\n",
            record_addr_range.to_string(), entry_size,
            record_addr_range.size() / 1024, max_record_entries);
    DPRINTF(HWPrefetch,
            "Replay mem cfg: range: %s"
            " entry size: %iB => capacity: %ikB = %i entries\n",
            replay_addr_range.to_string(), entry_size,
            replay_addr_range.size() / 1024, max_replay_entries);
}

bool MemoryInterface::writeRecord(BufferEntryPtr entry) {
    // Calculate the address in the record memory
    Addr addr = record_addr_range.start() + (recordIdx * entry_size);

    if (!record_addr_range.contains(addr)) {
        warn_once("Record memory space is full. Drop record.\n");
        // DPRINTF(HWPrefetch,"Record memory space is full. Drop record.\n");
        return false;
    }

    if (effective_bytes_written >= trace_size) {
        warn_once("Trace size reached. Drop record.\n");
        // DPRINTF(HWPrefetch,"Record memory space is full. Drop record.\n");
        return false;
    }

    // Create a data buffer and write the entry to it.
    // uint8_t* data = new uint8_t[entry_size];
    // entry->writeTo(data);
    uint8_t* data = serialize(entry);

    DPRINTF(HWPrefetch, "WR %#x -> [%s] to MEM. (Nr. %i)\n", addr,
            entry->print(), totalRecordEntries + 1);

    // Define what happens when the response is received.
    auto completeEvent = new EventFunctionWrapper(
        [this, addr, data]() {
            writeRecordComplete(addr, curTick());
            delete[] data;
        },
        name(), true);

    parent.port.dmaAction(MemCmd::WriteReq, addr, entry_size, completeEvent,
                          data, 0);
    // Entry is written increment record idx
    recordIdx++;
    totalRecordEntries++;

    effective_bytes_written += ceil(float(effectiveEntrySize) / 8);
    stats.effBytesWritten +=
        ceil(float(effectiveEntrySize) / 8);

    return true;
}

void MemoryInterface::writeRecordComplete(Addr addr, Tick wr_start) {
    stats.totalWrites++;
    stats.bytesWritten += entry_size;
    stats.totalWriteLatency += curTick() - wr_start;

    DPRINTF(HWPrefetch, "WR %#x complete\n", addr);
}

bool MemoryInterface::writeCacheLine(
    std::vector<BufferEntryPtr>& entries) {
    // Calculate the address in the record memory
    Addr addr = record_addr_range.start() + (recordCLIdx * blk_size);

    if (!record_addr_range.contains(addr)) {
        warn_once("Record memory space is full. Drop record.\n");
        // DPRINTF(HWPrefetch,"Record memory space is full. Drop record.\n");
        return false;
    }

    if (effective_bytes_written >= trace_size) {
        warn_once("Trace size reached. Drop record.\n");
        // DPRINTF(HWPrefetch,"Record memory space is full. Drop record.\n");
        return false;
    }

    // Create a data buffer for the cache line write the entries into it.
    uint8_t* data = new uint8_t[blk_size];

    // First we write the meta data in the first byte.
    uint8_t entry_type = entries[0]->_inst ? 0 : 1;
    data[0] = entry_type;
    int meta_size = 1;
    uint actual_bits = 1;

    // 2. Parse write the entries into the cache line
    for (int idx = 0; idx < entries.size(); idx++) {
        int bufferAddr = idx * entry_size + meta_size;
        assert(bufferAddr + entry_size <= blk_size);
        serialize(entries[idx], &data[bufferAddr]);
        DPRINTF(HWPrefetch, "WR %#x -> [%s] to MEM. (Nr. %i)\n", addr,
                entries[idx]->print(), totalRecordEntries + 1);
        totalRecordEntries++;
        actual_bits += entries[idx]->mask_bits + entries[idx]->mask_bits;
    }

    effective_bytes_written += ceil(float(actual_bits) / 8);
    stats.effBytesWritten += ceil(float(actual_bits) / 8);

    // Define what happens when the response is received.
    auto completeEvent = new EventFunctionWrapper(
        [this, addr, data]() {
            writeCacheLineComplete(addr, curTick());
            delete[] data;
        },
        name(), true);

    parent.port.dmaAction(MemCmd::WriteReq, addr, blk_size, completeEvent, data,
                          0);
    // Entry is written increment record idx
    recordCLIdx++;
    return true;
}

void MemoryInterface::writeCacheLineComplete(Addr addr,
                                                      Tick wr_start) {
    stats.totalWrites++;
    stats.bytesWritten += blk_size;
    stats.totalWriteLatency += curTick() - wr_start;

    DPRINTF(HWPrefetch, "WR %#x complete\n", addr);
}

bool MemoryInterface::readRecord() {
    if (replayIdx >= totalReplayEntries) {
        return false;
    }
    // Calculate the address in the replay memory
    Addr addr = replay_addr_range.start() + (replayIdx * entry_size);

    // Create a data buffer where the data can be written.
    uint8_t* data = new uint8_t[entry_size];

    DPRINTF(HWPrefetch, "RD %#x <- from MEM (%i/%i)\n", addr, replayIdx,
            totalReplayEntries);

    // Define what happens when the response is received.
    auto completeEvent = new EventFunctionWrapper(
        [this, addr, data]() {
            readRecordComplete(addr, data, curTick());
            delete[] data;
        },
        name(), true);

    parent.port.dmaAction(MemCmd::ReadReq, addr, entry_size, completeEvent,
                          data, 0);

    // Entry is requested increment replay idx.
    ++replayIdx;
    return true;
}

void MemoryInterface::readRecordComplete(Addr addr, uint8_t* data,
                                                  Tick rd_start) {
    // Upon receiving a read packet from memory we first update the stats.
    stats.totalReads++;
    stats.bytesRead += entry_size;
    stats.totalReadLatency += curTick() - rd_start;

    // // Now create a new buffer entry and copy the data into it.
    // auto newEntry = std::make_shared<BufferEntry>();
    // newEntry->readFrom(data);
    auto newEntry = deserialize(data);

    // delete data;
    // Finally fill the entry in the replay buffer.
    parent.replayBuffer->fill(newEntry);
    DPRINTF(HWPrefetch, "RD %#x -> [%s] complete (%i/%i)\n", addr,
            newEntry->print(), replayIdx, totalReplayEntries);
}

bool MemoryInterface::readRecordMem(
    std::vector<BufferEntryPtr>& entries) {
    if (totalRecordEntries <= 0) {
        warn("IStream prefetcher has not recorded any entry.\n");
        return false;
    }
    uint64_t size = totalRecordEntries * entry_size;
    uint8_t* buffer = new uint8_t[size];
    Addr addr = record_addr_range.start();

    // 1. Read the raw data from the memory into the prepared buffer.
    memProxy->readBlob(addr, buffer, size);

    // 2. Parse the raw data and pass the entries back to the writting function
    for (int idx = 0; idx < totalRecordEntries; idx++) {
        // Calculate the address in the record buffer
        int bufferAddr = idx * entry_size;

        // auto entry = std::make_shared<BufferEntry>();
        // entry->readFrom(&buffer[bufferAddr]);
        auto entry = deserialize(&buffer[bufferAddr]);

        DPRINTF(HWPrefetch, "Copy [%s]: from idx: %#x\n", entry->print(),
                bufferAddr);

        entries.push_back(entry);
    }
    delete[] buffer;
    return true;
}

bool MemoryInterface::readRecordMem2(
    std::vector<BufferEntryPtr>& entries) {
    if (totalRecordEntries <= 0) {
        warn("IStream prefetcher has not recorded any entry.\n");
        return false;
    }
    uint64_t size = totalRecordCLs * parent.blkSize;
    uint8_t* buffer = new uint8_t[size];
    Addr addr = record_addr_range.start();

    // 1. Read the raw data from the memory into the prepared buffer.
    memProxy->readBlob(addr, buffer, size);

    // 2. Parse the raw data and pass the entries back to the writing function
    for (int idx = 0; idx < totalRecordEntries; idx++) {
        // Calculate the address in the record buffer
        int bufferAddr = idx * entry_size;

        // auto entry = std::make_shared<BufferEntry>();
        // entry->readFrom(&buffer[bufferAddr]);
        auto entry = deserialize(&buffer[bufferAddr]);

        DPRINTF(HWPrefetch, "Copy [%s]: from idx: %#x\n", entry->print(),
                bufferAddr);

        entries.push_back(entry);
    }
    delete[] buffer;
    return true;
}

bool MemoryInterface::initReplayMem(
    std::vector<BufferEntryPtr>& trace) {
    totalReplayEntries = trace.size();

    bool ret_val = true;
    int max_entries = replay_addr_range.size() / entry_size;
    if (totalReplayEntries > max_entries) {
        totalReplayEntries = max_entries;
        warn(
            "Given address space is not large enough. "
            "Will only write the first %i entries.\n",
            totalReplayEntries);
        ret_val = false;
    }

    // Prepare a buffer
    uint64_t size = totalReplayEntries * entry_size;
    uint8_t* buffer = new uint8_t[size];

    // Write the initialisation entries to it.
    for (int idx = 0; idx < totalReplayEntries; idx++) {
        // Calculate the address in the replay memory
        int bufferAddr = idx * entry_size;

        // trace[idx]->writeTo(&buffer[bufferAddr]);
        serialize(trace[idx], &buffer[bufferAddr]);

        DPRINTF(HWPrefetch, "Copy [%s]: to address: %#x\n", trace[idx]->print(),
                bufferAddr);
    }

    // Now send the data to memory
    Addr addr = replay_addr_range.start();
    memProxy->writeBlob(addr, buffer, size);
    delete[] buffer;

    // Reset the replay pointer to the beginning.
    replayIdx = 0;

    DPRINTF(HWPrefetch, "Initializing replay memory complete. (%i entries).\n",
            totalReplayEntries);
    return ret_val;
}

DrainState MemoryInterface::drain() {
    // // All blocked packtets need to be send.
    // if (!blockedPkts.empty()) {
    //   DPRINTF(HWPrefetch, "Not all blocked packets are send yet.\n");
    //   return DrainState::Draining;
    // }
    // // All responses need to received.
    // if (!waitingResp.empty()) {
    //   DPRINTF(HWPrefetch, "Not all waiting responses received yet.\n");
    //   return DrainState::Draining;
    // }
    return DrainState::Drained;
}





/*******************************************************************
 *
 *  The Main Stream prefetcher.
 * Wraps around other classes and coordinates communication
 *
 *******************************************************************/





// void IStream::CacheListener::notify(const PacketPtr& pkt) {
//     if (isLevel1) {
//         parent.notifyFromLevel1(pkt, miss);
//     } else {
//         parent.notifyFromLevel2(pkt, miss);
//     }
// }

void IStream::notifyFromLevel1(const PacketPtr& pkt, bool miss) {
    notifyRecord(pkt, miss);
}

void IStream::notifyFromLevel2(const PacketPtr& pkt, bool miss) {
    notifyReplay(pkt, miss);
}

IStream::IStream(const Params& p)
    : Queued(p),

      // recordStream(nullptr),
      // replayStream(nullptr),
      memInterface(nullptr),
      port(this, p.sys),
      // backdoor(name() + "-backdoor", *this),
      // dmaPort(this, p.sys),
      enable_record(false),
      enable_replay(false),
      startOnCS(p.start_on_context_switch),
      stopOnCS(p.stop_on_context_switch),
      waitForCSActive(false),
      waitForCSIdle(false),
      recordHitInTargetCache(p.rec_hit_in_target_cache),
      recordHitInListCache(p.rec_hit_in_listener_cache),
      recordHitOnPfTargetCache(p.rec_hit_on_pf_target_cache),
      recordHitOnPfListCache(p.rec_hit_on_pf_listener_cache),
      recNAtMiss(p.rec_n_at_miss),
      regionSize(p.region_size),
      historyBits(p.history_bits),
      pfRecorded(0),
      pfReplayed(0),
      totalPrefetches(0),
      pfIssued(0),
      pfUsed(0),
      pfUnused(0),
      replayDistance(p.replay_distance),
      l1Cache(nullptr),
      l2Cache(nullptr),
      contextSwitchHook(nullptr),
      listenerCache(nullptr),

      recordBuffer(p.recordLogic),
      replayBuffer(p.replayLogic)
      // recordStats(this, "recordStats")

      // recordBuffer(p.recordBuffer, *this, recordStats,
      //             p.record_buffer_entries,
      //             p.region_size, blkSize)

{
    lRegionSize = floorLog2(regionSize);
    blksPerRegion = regionSize / blkSize;

    // Calculate the real entry size not the one we have for modeling.
    int bits_region_ptr = 48 - lRegionSize;
    // entrySize_n_addr = ceil(float(bits_region_ptr) / 8);
    // entrySize_n_mask = ceil(float(blksPerRegion) / 8);
    // entrySize_n_hist = ceil(float(historyBits) / 8);

    // entrySize_n_addr = ceil(float(bits_region_ptr) / 32) * 4;
    // entrySize_n_mask = ceil(float(blksPerRegion) / 32) * 4;
    // entrySize_n_hist = ceil(float(historyBits) / 32) * 4;

    // realEntrySize = entrySize_n_addr + entrySize_n_mask + entrySize_n_hist;
    // effectiveEntrySize = bits_region_ptr + blksPerRegion + historyBits + 1;
    // // warn("Effective Entry size: %i, (%i + %i + %i)")

    // // Calculate how many entries fit into one CL.
    // entries_per_cl = blkSize / realEntrySize;

    // DPRINTF(HWPrefetch,
    //         "Cfg: region size: %i, blk size %i => "
    //         "%i blks/region => Entry size: %ib (%ib + %ib + %ib) "
    //         "Model: %iB (%iB + %iB + %iB)."
    //         " %i entries per CL\n",
    //         regionSize, blkSize, blksPerRegion, effectiveEntrySize,
    //         bits_region_ptr, blksPerRegion, historyBits + 1, realEntrySize,
    //         entrySize_n_addr, entrySize_n_mask, entrySize_n_hist,
    //         entries_per_cl);

    // recordStream = new TraceStream(p.trace_record_file);
    // replayStream = new TraceStream(p.trace_replay_file);
    memInterface =
        new MemoryInterface(*this, p.record_addr_range, p.replay_addr_range,
                            p.trace_size, p.sys->physProxy,
                            this->name() + ".memIf");
    // typedef IStreamRecordBufferParams Params;
    // const IStreamRecordLogicParams &p2 =
    //     dynamic_cast<const IStreamRecordLogicParams &>(params());
    // // dynamic_cast<const IStreamRecordLogicParams &>(params()

    // recordBuffer =
    //     new RecordBuffer(p.recordBuffer,
    //                     *this, recordStats, p.record_buffer_entries,
    //                      regionSize, blkSize, this->name() + ".recBuffer");
    // const IStreamRecordBufferParams &p =
    //     dynamic_cast<const IStreamRecordBufferParams &>(params());
    // recordLogic = new RecordLogic(
    //                   dynamic_cast<const IStreamRecordLogicParams &>(params()),
    //                   *this, recordStats, p.record_buffer_entries,
    //                   regionSize, blkSize, this->name() + ".recBuffer");

    // replayBuffer =
    //     new ReplayBuffer(p.replay_buffer_entries,
    //                      regionSize, blkSize, this->name() + ".replayBuffer");

    recordBuffer->setParent(this);
    replayBuffer->setParent(this);

    // Register a callback to compensate for the destructor not
    // being called. The callback forces the stream to flush and
    // closes the output file.
    // registerExitCallback([this]() { closeStreams(); });
}

void IStream::init() {
    if (!port.isConnected())
        fatal("IStream prefetcher need to be connected to memory.\n");
}

DrainState IStream::drain() {
    // Before reading and writting traces the system and prefetcher must be
    // in drained state.
    // for draining the simulator will repead to call this function until
    // we return 'Drained'
    //
    // To be drained we want:
    // 1. All record buffer entries to be send to memory
    // 2. All queues in the memory interface to be empty
    //
    return DrainState::Drained;
    // If recordBuffer not empty
    // if (!recordBuffer->empty()) {
    //   DPRINTF(HWPrefetch, "Record Buffer not empty. "
    //                          "Flush content to memory.\n");
    //   // flush all content.
    //   recordBuffer->flush(true);
    //   // Disable the prefetcher
    //   enable_record = false;
    //   enable_replay = false;
    //   return DrainState::Draining;
    // }

    // Wait for the MemIF to be drained as well.
    // return memInterface->drain();
    // return DrainState::Drained;
}

/********************************************************
 *         I-Stream prefetcher aka. Jukebox
 ********************************************************
 * This prefetcher is specially designed to limit the
 * impact of cold hardware structures we investigate
 * for serverless functions comming from the fact the
 * 100s to thousands of functions are scheduled on the
 * same CPU core and there are a lot of context switches
 * between the different functions.
 *
 * The Instruction stream prefetcher exists of two part:
 *
 * - Record:
 *    This component track instruction accesses and store
 *    them in compressed form in a trace.
 *    Note this is a prefetcher designed specifically for
 *    optimizing start ups and cold starts that means as
 *    soon as the trace cannot hold more entries the
 *    prefetcher will stop recording.
 *
 * - Replay:
 *    This component is independent of the recording.
 *    It will read a prevously recorded trace, create
 *    prefetches to the prevously recorded addresse in order
 *    to basically restore the cache lines we loose while
 *    the function was descheduled.
 */

/*
 * RECORD ---------------------------------------------
 */

void IStream::squashPrefetches(Addr addr, bool is_secure) {
    // Squash queued prefetches if demand miss to same line
    if (queueSquash) {
        auto itr = pfq.begin();
        while (itr != pfq.end()) {
            if (itr->pfInfo.getAddr() == addr &&
                itr->pfInfo.isSecure() == is_secure) {
                DPRINTF(HWPrefetch,
                        "Removing pf candidate addr: %#x "
                        "(cl: %#x), demand request going to the same addr\n",
                        itr->pfInfo.getAddr(),
                        blockAddress(itr->pfInfo.getAddr()));
                delete itr->pkt;
                itr = pfq.erase(itr);
                statsQueued.pfRemovedDemand++;
            } else {
                ++itr;
            }
        }
    }
}

bool IStream::checkPacket(const PacketPtr& pkt) {
    // Don't notify prefetcher on SWPrefetch, cache maintenance
    // operations or for writes that we are coaslescing.
    if (pkt->cmd.isSWPrefetch()) return false;
    if (pkt->req->isCacheMaintenance()) return false;
    if (pkt->isWrite() && cache != nullptr && cache->coalesce()) return false;
    if (!pkt->req->hasPaddr()) {
        panic("Request must have a physical address");
    }
    // Also do not notify clean evictions or invalidation requests
    if (pkt->req->isUncacheable()) return false;
    if (pkt->cmd == MemCmd::CleanEvict) return false;
    if (pkt->isInvalidate()) return false;
    if (useVirtualAddresses && (tlb == nullptr)) {
        DPRINTF(HWPrefetch,
                "Using virtual for Record and replay "
                " requires to register a TLB.\n");
        return false;
    }
    return true;
}

void IStream::notifyRecord(const PacketPtr& pkt, bool miss) {
    if (!checkPacket(pkt)) return;

    Addr blk_addr = 0;
    if (useVirtualAddresses && pkt->req->hasVaddr()) {
        blk_addr = blockAddress(pkt->req->getVaddr());
    } else if (!useVirtualAddresses) {
        blk_addr = blockAddress(pkt->req->getPaddr());
    } else {
        return;
    }

    // Check if this access should be recorded
    auto accessType = observeAccess(pkt, miss);
    if (filterAccessTypes(accessType)) {
        record(blk_addr, accessType);
    }
}

void IStream::probeNotify(const PacketPtr& pkt, bool miss) {}

bool IStream::filterAccessTypes(IStream::AccessType accType) {
    switch (accType) {
        case AccessType::HIT_L1_PF:
            if (recordHitOnPfListCache) return true;
            break;

        case AccessType::HIT_L2_PF:
            if (recordHitOnPfTargetCache) return true;
            break;

        case AccessType::HIT_L2:
            if (recordHitInTargetCache) return true;
            break;

        case AccessType::MISS_L2:
            return true;

        default:
            break;
    }
    return false;
}

IStream::AccessType IStream::observeAccess(const PacketPtr& pkt, bool miss) {
    if (!enable_record) return INV;

    // For checking the caches we need the physical address
    Addr pBlkAddr = blockAddress(pkt->req->getPaddr());

    bool inst = pkt->req->isInstFetch();
    bool read = pkt->isRead();
    bool inv = pkt->isInvalidate();
    bool prefetch = pkt->req->isPrefetch();
    bool rdClean = pkt->cmd == MemCmd::ReadCleanReq;
    bool hitOnPrefetch_l2 =
        l2Cache->hasBeenPrefetched(pBlkAddr, pkt->isSecure());
    bool hit_l2 = l2Cache->inCache(pBlkAddr, pkt->isSecure());
    bool inMissq_l2 = l2Cache->inMissQueue(pBlkAddr, pkt->isSecure());

    // We can configure the prefetcher to get notifications from another
    // cache level. In this case we listen probe this cache as well.
    bool hitOnPrefetch_l1 =
        (l1Cache) ? l1Cache->hasBeenPrefetched(pBlkAddr, pkt->isSecure())
                  : false;
    bool hit_l1 =
        (l1Cache) ? l1Cache->inCache(pBlkAddr, pkt->isSecure()) : false;
    bool inMissq_l1 =
        (l1Cache) ? l1Cache->inMissQueue(pBlkAddr, pkt->isSecure()) : false;

    // bool hitOnPrefetch_l1 = (l1Cache) ?
    //       l1Cache->hasBeenPrefetched(pBlkAddr, pkt->isSecure()) : false;
    // bool hit_l1 = (l1Cache) ?
    //       l1Cache->inCache(pBlkAddr, pkt->isSecure()) : false;
    // bool inMissq_l1 = (l1Cache) ?
    //       l1Cache->inMissQueue(pBlkAddr, pkt->isSecure()) : false;

    // if (pkt->req->isUncacheable()) return false;
    // if (fetch && !onInst) return false;
    // if (!fetch && !onData) return false;
    // if (!fetch && read && !onRead) return false;
    // if (!fetch && !read && !onWrite) return false;
    // if (!fetch && !read && inv) return false;
    // if (pkt->cmd == MemCmd::CleanEvict) return false;

    recordBuffer->stats.notifies++;
    if (prefetch) recordBuffer->stats.pfRequest++;
    if (inst) recordBuffer->stats.instRequest++;
    if (rdClean) recordBuffer->stats.readCleanReq++;
    // Only record instructions
    // if (!inst && !rdClean) return INV;

    if (inst && !onInst) return INV;
    if (!inst && !onData) return INV;

    // && !onInst
    // if (!inst) return INV;

    AccessType access = INV;
    if (!hit_l1) {
        recordBuffer->stats.missL1Cache++;
        // Misses in the listener cache need to be recorded
        // Except:
        // Accesses that are already cached in the target cache This probing is
        // necessary because we can configure the
        // recording to sit in another cache level then the target cache.
        // In this case the misses we get are misses from the listening cache.
        // But they might hit in the target cache.
        // However, in case the

        // A miss can furthermore hit in the target cache,
        // hit on a pf in the target cache or miss
        //
        if (hitOnPrefetch_l2) {
            recordBuffer->stats.hitOnPrefetchInL2++;
            recordBuffer->stats.hitL2Cache++;
            access = HIT_L2_PF;
        } else if (hit_l2) {
            recordBuffer->stats.hitL2Cache++;
            access = HIT_L2;
        } else {
            recordBuffer->stats.missL2Cache++;
            access = MISS_L2;
        }
    } else {
        // Hit in listener cache or a prefetch in the listener
        // cache.
        if (hitOnPrefetch_l1) {
            recordBuffer->stats.hitOnPrefetchInL1++;
            recordBuffer->stats.hitL1Cache++;
            access = HIT_L1_PF;
        } else {
            recordBuffer->stats.hitL1Cache++;
            access = HIT_L1;
        }
    }

    // // Only record instructions
    // if (!rdClean) rec_addr = false;

    DPRINTF(HWPrefetch, "Notify: A: %#x [%s|%s|%s| %s,%s,%s,%s]\n",
            pkt->getAddr(), miss ? "miss" : "hit", inst ? "inst" : "data",
            read ? "rd" : "wr", prefetch ? "pf " : "",
            hitOnPrefetch_l2 ? "hitpf " : "", inMissq_l2 ? "hitMq " : " ");

    // Update the access statistics
    // if (!miss) recordBuffer->stats.cacheHit++;
    // if (prefetch) recordBuffer->stats.pfRequest++;
    // if (inst) recordBuffer->stats.demandRequest++;
    // if (hitOnPrefetch_l2) recordBuffer->stats.hitOnPrefetchInL2++;
    // if (cached_target) recordBuffer->stats.hitL2Cache++;
    if (inMissq_l2) recordBuffer->stats.hitInL2MissQueue++;
    // if (cached_listener) recordBuffer->stats.hitL1Cache++;
    if (inMissq_l1) recordBuffer->stats.hitInL1MissQueue++;
    // if (hitOnPrefetch_l1) recordBuffer->stats.hitOnPrefetchInL1++;

    return access;
}

void IStream::record(const Addr addr, AccessType accType) {
    Addr blkAddr = blockAddress(addr);
    DPRINTF(HWPrefetch, "Record blk access [%#x]\n", blkAddr);
    recordBuffer->access(addr, curCycle());

    if (recNAtMiss > 0) {
        recordBuffer->access(addr + blkSize, curCycle());
    }
    recordBuffer->flush();
}

// void IStream::calculatePrefetch(const PrefetchInfo& pfi,
//                                 std::vector<AddrPriority>& addresses) {
//     //   if (enable_record) {
//     //     record(pfi);
//     //   }
// }

/*
 * REPLAY ----------------------------------------------
 */

void IStream::notifyReplay(const PacketPtr& pkt, bool miss) {
    if (!checkPacket(pkt)) return;

    // The I-Stream prefetcher is a pure instruction
    // prefetcher
    // bool inst = pkt->req->isInstFetch();
    // if (!inst)
    //     return;

    // Update prefetch statistics
    if (hasBeenPrefetched(pkt->getAddr(), pkt->isSecure())) {
        usefulPrefetches += 1;
        prefetchStats.pfUseful++;
        // TODO:
        replayBuffer->stats.pfUseful++;
        pfUsed++;
        if (miss) {
            // This case happens when a demand hits on a prefetched line
            // that's not in the requested coherency state.
            prefetchStats.pfUsefulButMiss++;
            replayBuffer->stats.pfUsefulButMiss++;
        }
    }

    // Create prefetch information
    Addr blk_addr = 0;
    if (useVirtualAddresses && pkt->req->hasVaddr()) {
        blk_addr = blockAddress(pkt->req->getVaddr());
    } else if (!useVirtualAddresses) {
        blk_addr = blockAddress(pkt->req->getPaddr());
    } else {
        return;
    }
    PrefetchInfo pfi(pkt, blk_addr, miss);

    // Incase we see a request to the same address the prefetch
    // would be to late.
    squashPrefetches(blk_addr, pfi.isSecure());

    // Now do the actual replay
    // Only if enabled and only as may as fit into the prefetch queue.
    int max = queueSize - pfq.size() - pfqMissingTranslation.size();
    if (!enable_replay || max < 1) return;

    // Generate the next addresses from the replay buffer
    std::vector<Addr> addresses;
    replay(addresses);

    // Queue up generated prefetches
    for (Addr& addr : addresses) {
        // Block align prefetch address
        addr = blockAddress(addr);
        if (!samePage(addr, pfi.getAddr())) {
            statsQueued.pfSpanPage += 1;
        }

        PrefetchInfo new_pfi(pfi, addr);
        statsQueued.pfIdentified++;
        DPRINTF(HWPrefetch,
                "Found a pf candidate addr: %#x, "
                "inserting into prefetch queue.\n",
                new_pfi.getAddr());

        // Create and insert the request
        const int32_t prio = 0;
        insert(pkt, new_pfi, prio);
    }

    // Now translation can be started if necessary.
    processMissingTranslations(queueSize - pfq.size());
}

void IStream::replay(std::vector<Addr>& addresses) {
    // Pace down replay in case we are to far ahead.
    // This will avoid thrashing the cache.
    if (replayDistance >= 0) {
        DPRINTF(HWPrefetch,
                "Steer replay distance: "
                "Recorded/Replayed/Issued/Used/Unused prefetches: "
                "%u/%u/%u/%u/%u\n",
                pfReplayed, pfRecorded, pfIssued, pfUsed, pfUnused);
        if (pfIssued >= pfUsed + pfUnused + replayDistance) {
            DPRINTF(HWPrefetch, "Slow down replaying.\n");
            return;
        }
    }

    if (!replayBuffer->refill()) {
        DPRINTF(HWPrefetch,
                "Reached end of trace file. No more records "
                "for prefetching \n");
        if (pfReplayed >= totalPrefetches) {
            DPRINTF(HWPrefetch, "All prefetches issued. Replaying done!!\n");
            enable_replay = false;
            return;
        }
        DPRINTF(HWPrefetch, "Replay buffer still contains entries\n");
    }

    // In case there is an entry in the reply buffer for this address
    // then we are already to late. We want to fetch the other addresses as
    // soon as possible.

    DPRINTF(HWPrefetch, "pfq size: %u, pfqMissingTr size: %u\n", pfq.size(),
            pfqMissingTranslation.size());

    DPRINTF(HWPrefetch, "Repl. Buff: %s\n", replayBuffer->print());
    // Generate new prefetch addresses.
    int max = queueSize - pfq.size() - pfqMissingTranslation.size();
    // At this point we should be able insert at least one address into
    // the queues
    assert(max > 0);

    addresses = replayBuffer->generateNAddresses(max);
    pfReplayed += addresses.size();
    replayBuffer->stats.pfGenerated += addresses.size();
    DPRINTF(HWPrefetch, "Generated %i new pref. cand. (max: %i) (%i/%i)\n",
            addresses.size(), max, pfReplayed, totalPrefetches);

    // DPRINTF(HWPrefetch, "Repl. Buff: %s\n", replayBuffer->print());
}

void IStream::notify(const PacketPtr& pkt, const PrefetchInfo& pfi) {}

void IStream::proceedWithTranslation(const PacketPtr& pkt,
                                     PrefetchInfo& new_pfi, int32_t priority) {
    /*
     * Physical address computation
     * As we record addresses we only need to compute the PA
     * in case we recorded virtual once.
     */

    // ContextID is needed for translation
    if (!pkt->req->hasContextId()) {
        return;
    }

    RequestPtr translation_req =
        createPrefetchRequest(new_pfi.getAddr(), new_pfi, pkt);

    /* Create the packet and find the spot to insert it */
    DeferredPacket dpp(this, new_pfi, 0, priority);

    // Add the translation request.
    dpp.setTranslationRequest(translation_req);
    dpp.tc = cache->system->threads[translation_req->contextId()];
    DPRINTF(HWPrefetch,
            "Prefetch queued with no translation. "
            "addr:%#x priority: %3d\n",
            new_pfi.getAddr(), priority);
    addToQueue(pfqMissingTranslation, dpp);
}

void IStream::insert(const PacketPtr& pkt, PrefetchInfo& new_pfi,
                     int32_t priority) {
    if (queueFilter) {
        if (alreadyInQueue(pfq, new_pfi, priority)) {
            return;
        }
        if (alreadyInQueue(pfqMissingTranslation, new_pfi, priority)) {
            return;
        }
    }

    /* In case we work with virtual addresses we need to translate first */
    if (useVirtualAddresses) {
        proceedWithTranslation(pkt, new_pfi, priority);
        return;
    }

    // Using PA: no translation necessary.
    Addr target_paddr = new_pfi.getAddr();

    if (cacheSnoop && (inCache(target_paddr, new_pfi.isSecure()) ||
                       inMissQueue(target_paddr, new_pfi.isSecure()))) {
        statsQueued.pfInCache++;
        replayBuffer->stats.pfHitInCache++;
        DPRINTF(HWPrefetch,
                "Dropping redundant in "
                "cache/MSHR prefetch addr:%#x\n",
                target_paddr);
        return;
    }

    /* Create the packet and insert it in the prefetch queue */
    DeferredPacket dpp(this, new_pfi, 0, priority);

    Tick pf_time = curTick() + clockPeriod() * latency;
    dpp.createPkt(target_paddr, blkSize, requestorId, tagPrefetch, pf_time);
    DPRINTF(HWPrefetch,
            "Prefetch queued. "
            "addr:%#x priority: %3d tick:%lld.\n",
            new_pfi.getAddr(), priority, pf_time);

    replayBuffer->stats.pfInsertedInQueue++;
    addToQueue(pfq, dpp);
}

void IStream::translationComplete(DeferredPacket* dp, bool failed) {
    auto it = pfqMissingTranslation.begin();
    while (it != pfqMissingTranslation.end()) {
        if (&(*it) == dp) {
            break;
        }
        it++;
    }
    assert(it != pfqMissingTranslation.end());
    if (!failed) {
        DPRINTF(HWPrefetch,
                "%s Translation of vaddr %#x succeeded: "
                "paddr %#x \n",
                tlb->name(), it->translationRequest->getVaddr(),
                it->translationRequest->getPaddr());
        Addr target_paddr = it->translationRequest->getPaddr();
        // check if this prefetch is already redundant
        if (cacheSnoop && (inCache(target_paddr, it->pfInfo.isSecure()) ||
                           inMissQueue(target_paddr, it->pfInfo.isSecure()))) {
            statsQueued.pfInCache++;
            replayBuffer->stats.pfHitInCache++;
            DPRINTF(HWPrefetch,
                    "Dropping redundant in "
                    "cache/MSHR prefetch addr:%#x\n",
                    target_paddr);
        } else {
            Tick pf_time = curTick() + clockPeriod() * latency;
            it->createPkt(it->translationRequest->getPaddr(), blkSize,
                          requestorId, tagPrefetch, pf_time);
            replayBuffer->stats.pfInsertedInQueue++;
            addToQueue(pfq, *it);
        }

    } else {
        DPRINTF(HWPrefetch,
                "%s Translation of vaddr %#x failed, dropping "
                "prefetch request. \n",
                tlb->name(), it->translationRequest->getVaddr());
        // statsQueued.pfTranslationFail++;
        replayBuffer->stats.pfTranslationFail++;
        // replayBuffer->stats.translationRetries++;
        // We will leave the deferred packet in the outstanding queue
        // and try to do the translation again.
    }
    pfqMissingTranslation.erase(it);
}


void IStream::startRecord(bool wait_for_cs) {
    // if (wait_for_cs) {
    //   waitForCSActive = true;
    //   return;
    // }
    DPRINTF(HWPrefetch, "Start Recoding instructions\n");
    // recordStream->reset();
    recordBuffer->clear();
    memInterface->resetRecord();
    pfRecorded = 0;
    enable_record = true;
}

bool IStream::startReplay(bool wait_for_cs) {
    if (!memInterface->replayReady()) {
        warn(
            "Cannot start replay because memory is not "
            "completely initialized.\n");
        return false;
    }
    // if (wait_for_cs) {
    //   waitForCSActive = true;
    //   return true;
    // }

    DPRINTF(HWPrefetch, "Start Replaying instructions\n");
    // recordStream->reset();
    memInterface->resetReplay();
    replayBuffer->clear();
    replayBuffer->refill();
    pfReplayed = 0;
    pfIssued = 0;
    pfUsed = 0;
    pfUnused = 0;
    enable_replay = true;
    return true;
}

// void
//   IStream::startAtScheduling()
// {
//   waitForCSActive = true;
//   waitForCSIdle = false;
// }

std::string IStream::readFileIntoString(const std::string& filename) {
    auto ss = std::ostringstream{};
    std::ifstream input_file(filename);
    if (!input_file.is_open()) {
        std::cerr << "Could not open the file - '" << filename << "'"
                  << std::endl;
        exit(EXIT_FAILURE);
    }
    ss << input_file.rdbuf();
    return ss.str();
}

void IStream::initReplay(std::string filename) {
    std::string file_contents = readFileIntoString(filename);
    std::vector<BufferEntryPtr> trace;

    std::istringstream sstream(file_contents);
    std::string line;
    totalPrefetches = 0;
    while (std::getline(sstream, line)) {
        // File was in binary format.
        // fileStream.read((char*)entry, sizeof(*entry));

        // File was in human readable format.
        auto tmp = std::make_shared<BufferEntry>();

        if (!tmp->parseLine(line)) {
            break;
        }
        totalPrefetches += tmp->bitsSet();
        trace.push_back(tmp);
    }
    replayBuffer->stats.totalPrefetchesInTrace += totalPrefetches;

    DPRINTF(HWPrefetch,
            "Read %i entries from %s: Init mem. Trace contains "
            "%i prefetches\n",
            trace.size(), filename, totalPrefetches);
    // Write the entries to memory
    if (!memInterface->initReplayMem(trace)) {
        warn("Could not initialize replay memory correctly.\n");
    }
}

void IStream::dumpRecTrace(std::string filename) {
    // Make sure the content is fully written to memory.
    recordBuffer->flush(true);

    // Get the entries from the memory interface.
    std::vector<BufferEntryPtr> entries;
    memInterface->readRecordMem(entries);

    // Write the entries into the give file
    std::fstream fileStream(filename.c_str(), std::ios::out | std::ios::trunc);
    if (!fileStream.good()) {
        panic("Could not open %s\n", filename);
    }

    DPRINTF(HWPrefetch, "Got %i entries from memory. Write them to %s.\n",
            entries.size(), filename);
    int num_prefetches = 0;
    for (auto it : entries) {
        num_prefetches += it->bitsSet();
        fileStream << it->toReadable() << "\n";
    }
    DPRINTF(HWPrefetch,
            "Record trace contains %i entries to generate %i "
            "prefetches. Write the trace to %s.\n",
            entries.size(), num_prefetches, filename);
    fileStream.close();
}

// void
// IStream::calcStackDistance(Addr addr)
// {
//   // Align the address to a cache line size
//   // const Addr aligned_addr(roundDown(pkt_info.addr, lineSize));
//   const Addr aligned_addr(addr);

//   // Calculate the stack distance
//   const uint64_t sd(sdcalc.calcStackDistAndUpdate(aligned_addr).first);
//   if (sd == StackDistCalc::Infinity) {
//       // stats.infiniteSD++;
//       DPRINTF(HWPrefetch, "InfiniteSD\n");
//       return;
//   }

//   Sample the stack distance of the address in lin and log bins
//   recordBuffer->stats.hitSDlinHist.sample(sd);
//   int sd_lg2 = sd == 0 ? 1 : floorLog2(sd);
//   recordBuffer->stats.hitSDlogHist.sample(sd_lg2);
// }

// void
// IStream::addEventProbeCS(SimObject *obj, const char *name)
// {
//     warn("Register HWP event probe %s for obj: %s", name, obj->name());
//     ProbeManager *pm(obj->getProbeManager());
//     listenersSC.push_back(new PrefetchListenerCS(*this, pm, name));
// }


void IStream::regProbeListeners() {
    // // First try to register the context switch hook
    // if (contextSwitchHook) {
    //   addEventProbeCS(contextSwitchHook, "SwitchActiveIdle");
    // }

    //
    /**
     * Try to register L1 and L2 caches
     * If no probes were added by the configuration scripts, connect to the
     * parent cache using the probe "Miss". Also connect to "Hit", if the
     * cache is configured to prefetch on accesses.
     */
    if (!l2Cache) fatal("IStream prefetcher need at least the L2 cache.\n");

    typedef ProbeListenerArgFn<PacketPtr> CacheListener;

    // ProbeManager* l2_pm(l2Cache->getProbeManager());
    // cacheListeners.push_back(
    //     new CacheListener(*this, l2_pm, "Miss", false, true));
    // cacheListeners.push_back(
    //     new CacheListener(*this, l2_pm, "Hit", false, false));

    // if (l1Cache) {
    //     ProbeManager* l1_pm(l1Cache->getProbeManager());
    //     cacheListeners.push_back(
    //         new CacheListener(*this, l1_pm, "Miss", true, true));
    //     cacheListeners.push_back(
    //         new CacheListener(*this, l1_pm, "Hit", true, false));
    // } else {
    //     warn("IStream prefetcher need the L1-I cache to be set.\n");
    // }
// void IStream::notifyFromLevel1(const PacketPtr& pkt, bool miss) {
//     notifyRecord(pkt, miss);
// }

    listeners.push_back(
            new CacheListener(l2Cache->getProbeManager(), "Miss",
                [this](const PacketPtr& pkt)
                    { notifyFromLevel2(pkt, true); }));
    listeners.push_back(
            new CacheListener(l2Cache->getProbeManager(), "Hit",
                [this](const PacketPtr& pkt)
                    { notifyFromLevel2(pkt, false); }));

    if (l1Cache) {
        listeners.push_back(
                new CacheListener(l1Cache->getProbeManager(), "Miss",
                    [this](const PacketPtr& pkt)
                        { notifyFromLevel1(pkt, true); }));
        listeners.push_back(
                new CacheListener(l1Cache->getProbeManager(), "Hit",
                    [this](const PacketPtr& pkt)
                        { notifyFromLevel1(pkt, false); }));
    }
    //     ProbeManager* l2_pm(l2Cache->getProbeManager());
    // cacheListeners.push_back(
    //     new CacheListener(*this, l2_pm, "Miss", false, true));
    // cacheListeners.push_back(
    //     new CacheListener(*this, l2_pm, "Hit", false, false));

    // if (l1Cache) {
    //     ProbeManager* l1_pm(l1Cache->getProbeManager());
    //     cacheListeners.push_back(
    //         new CacheListener(*this, l1_pm, "Miss", true, true));
    //     cacheListeners.push_back(
    //         new CacheListener(*this, l1_pm, "Hit", true, false));
    // } else {
    //     warn("IStream prefetcher need the L1-I cache to be set.\n");
    // }
}







Port& IStream::getPort(const std::string& if_name, PortID idx) {
    if (if_name == "port") {
        return port;
    }
    return ClockedObject::getPort(if_name, idx);
}



RecordBuffer::RecStats::RecStats(RecordBuffer* parent)
    : statistics::Group(parent),
      ADD_STAT(notifies, statistics::units::Count::get(),
               "Number of notification accesses the recording got."),
      ADD_STAT(hits, statistics::units::Count::get(),
               "Number of hits in the record buffer"),
      ADD_STAT(misses, statistics::units::Count::get(),
               "Number of misses in the record buffer"),
      ADD_STAT(cacheHit, statistics::units::Count::get(),
               "Number of accesses we might skip recording because it is a "
               "cache hit"),
      ADD_STAT(hitL1Cache, statistics::units::Count::get(),
               "Number of accesses we hit in the L1"),
      ADD_STAT(hitL2Cache, statistics::units::Count::get(),
               "Number of accesses we also hit in the L2"),
      ADD_STAT(missL1Cache, statistics::units::Count::get(),
               "Number of accesses we miss in the L1"),
      ADD_STAT(missL2Cache, statistics::units::Count::get(),
               "Number of accesses we miss in the L2"),
      ADD_STAT(hitInL1MissQueue, statistics::units::Count::get(),
               "Number of accesses hitting in the L1 MSHR queue"),
      ADD_STAT(hitInL2MissQueue, statistics::units::Count::get(),
               "Number of accesses hitting in the L2 MSHR queue"),
      ADD_STAT(hitOnPrefetchInL1, statistics::units::Count::get(),
               "Number of cache hits not skipped because they where useful "
               "cache hits."),
      ADD_STAT(hitOnPrefetchInL2, statistics::units::Count::get(),
               "Number of cache hits not skipped because they where useful "
               "cache hits."),
      ADD_STAT(instRequest, statistics::units::Count::get(),
               "Number of notf. from instruction request"),
      ADD_STAT(readCleanReq, statistics::units::Count::get(),
               "Number of notf. from data"),
      ADD_STAT(pfRequest, statistics::units::Count::get(),
               "Number of notifications from a prefetch request"),
      ADD_STAT(accessDrops, statistics::units::Count::get(),
               "Number of accesses we actually dropped due to 'cacheHit',"
               " 'inCache' or 'hitInMissQueue'"),
      ADD_STAT(bufferHitDistHist, statistics::units::Count::get(),
               "Record buffer hits distance distribution"),
      ADD_STAT(entryWrites, statistics::units::Count::get(),
               "Number of entries written to the record trace"),
      ADD_STAT(entryDrops, statistics::units::Count::get(),
               "Number of entires dropped to write to the record trace"),
      ADD_STAT(entryOverflows, statistics::units::Count::get(),
               "Number of entires not be write to the record because of "
               "limited memory capacity."),
      ADD_STAT(totalNumPrefetches, statistics::units::Count::get(),
               "Total number of prefetches the trace we recorded contains"),
      ADD_STAT(avgNumPrefetchesPerEntry, statistics::units::Count::get(),
               "Average number of prefetches within one entry.")
// ADD_STAT(hitSDlinHist, statistics::units::Count::get(),
//              "Hit stack distance linear distribution"),
// ADD_STAT(hitSDlogHist, statistics::units::Ratio::get(),
//              "Hit stack distance logarithmic distribution")
{
    using namespace statistics;

    const IStreamRecordLogicParams& p =
        dynamic_cast<const IStreamRecordLogicParams&>(parent->params());

    bufferHitDistHist
        .init((p.buffer_entries < 2)
                  ? 2
                  : ((p.buffer_entries > 16) ? 16 : p.buffer_entries))
        .flags(pdf);

    avgNumPrefetchesPerEntry.flags(total);
    avgNumPrefetchesPerEntry = totalNumPrefetches / entryWrites;

    // hitSDlinHist
    //     .init(16)
    //     .flags(pdf);

    // hitSDlogHist
    //     .init(32)
    //     .flags(pdf);
}

ReplayBuffer::ReplStats::ReplStats(ReplayBuffer* parent)
    : statistics::Group(parent),
      ADD_STAT(
          notifyHits, statistics::units::Count::get(),
          "Number of hits in of a cache notify access in the replay buffer."),
      ADD_STAT(pfGenerated, statistics::units::Count::get(),
               "Number of prefetches created from the trace."),
      ADD_STAT(pfInsertedInQueue, statistics::units::Count::get(),
               "Number of prefetches finally inserted in the pfq"),
      ADD_STAT(totalPrefetchesInTrace, statistics::units::Count::get(),
               "Total number of prefetches that the traces contain."),
      ADD_STAT(pfUseful, statistics::units::Count::get(),
               "Number of useful prefetches"),
      ADD_STAT(pfUsefulButMiss, statistics::units::Count::get(),
               "Number of useful prefetches but still miss in cache"),
      ADD_STAT(demandMshrMisses, statistics::units::Count::get(),
               "Number of MSHR demand misses"),
      ADD_STAT(
          pfBufferHit, statistics::units::Count::get(),
          "Number prefetches dropped because already in the prefetch buffer."),
      ADD_STAT(pfHitInCache, statistics::units::Count::get(),
               "number prefetches dropped because already in cache"),
      ADD_STAT(pfTranslationFail, statistics::units::Count::get(),
               "number prefetches dropped because already in cache"),
      ADD_STAT(pfDrops, statistics::units::Count::get(),
               "Total number of prefetches dropped."),
      ADD_STAT(accuracy, statistics::units::Count::get(),
               "Total number of prefetches dropped."),
      ADD_STAT(coverage, statistics::units::Count::get(),
               "Total number of prefetches dropped."),

      ADD_STAT(entryReads, statistics::units::Count::get(),
               "Number of entries read from the record trace"),
      // ADD_STAT(cumUsefullness, statistics::units::Count::get(),
      //   "Cummulative usefullness of the entries read from the trace."),
      ADD_STAT(avgPrefetchesPerEntry, statistics::units::Count::get(),
               "Average usefullness of the entries read from the trace.") {
    using namespace statistics;

    // const IStreamPrefetcherParams& p =
    //   dynamic_cast<const IStreamPrefetcherParams&>(parent->params());
    pfDrops.flags(total);
    pfDrops = pfBufferHit + pfHitInCache + pfTranslationFail;

    accuracy.flags(total);
    accuracy = pfUseful / pfInsertedInQueue;

    coverage.flags(total);
    coverage = pfUseful / (pfUseful + demandMshrMisses);

    avgPrefetchesPerEntry.flags(total);
    avgPrefetchesPerEntry = pfGenerated / entryReads;
}

MemoryInterface::MemIFStats::MemIFStats(IStream* parent,
                                              const std::string& name)
    : statistics::Group(parent, name.c_str()),
      ADD_STAT(numPackets, statistics::units::Count::get(),
               "Number of packets generated"),
      ADD_STAT(numRetries, statistics::units::Count::get(),
               "Number of retries"),
      ADD_STAT(retryTicks, statistics::units::Tick::get(),
               "Time spent waiting due to back-pressure"),
      ADD_STAT(bytesRead, statistics::units::Byte::get(),
               "Number of bytes read"),
      ADD_STAT(bytesWritten, statistics::units::Byte::get(),
               "Number of bytes written"),
      ADD_STAT(effBytesWritten, statistics::units::Byte::get(),
               "Number of effective bytes written"),
      ADD_STAT(totalReadLatency, statistics::units::Tick::get(),
               "Total latency of read requests"),
      ADD_STAT(totalWriteLatency, statistics::units::Tick::get(),
               "Total latency of write requests"),
      ADD_STAT(totalReads, statistics::units::Count::get(),
               "Total num of reads"),
      ADD_STAT(totalWrites, statistics::units::Count::get(),
               "Total num of writes"),
      ADD_STAT(avgReadLatency,
               statistics::units::Rate<statistics::units::Tick,
                                       statistics::units::Count>::get(),
               "Avg latency of read requests", totalReadLatency / totalReads),
      ADD_STAT(avgWriteLatency,
               statistics::units::Rate<statistics::units::Tick,
                                       statistics::units::Count>::get(),
               "Avg latency of write requests",
               totalWriteLatency / totalWrites),
      ADD_STAT(readBW,
               statistics::units::Rate<statistics::units::Byte,
                                       statistics::units::Second>::get(),
               "Read bandwidth", bytesRead / simSeconds),
      ADD_STAT(writeBW,
               statistics::units::Rate<statistics::units::Byte,
                                       statistics::units::Second>::get(),
               "Write bandwidth", bytesWritten / simSeconds) {}



// }  // namespace StreamPref
}  // namespace prefetch
}  // namespace gem5
