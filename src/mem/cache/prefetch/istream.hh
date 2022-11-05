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
 * Describes a IStream prefetcher.
 */

#ifndef __MEM_CACHE_PREFETCH_ISTREAM_HH__
#define __MEM_CACHE_PREFETCH_ISTREAM_HH__

#include <iostream>
#include <vector>

#include "debug/HWPrefetch.hh"
#include "dev/dma_device.hh"
#include "mem/cache/base.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/cache/prefetch/associative_set.hh"
#include "mem/packet.hh"
// #include "mem/port.hh"
#include "mem/request.hh"
#include "mem/stack_dist_calc.hh"
#include "base/circular_queue.hh"



#include "params/IStreamPrefetcher.hh"
#include "params/IStreamRecordLogic.hh"
#include "params/IStreamReplayLogic.hh"


namespace gem5 {


struct IStreamPrefetcherParams;
struct IStreamRecordLogicParams;
struct IStreamReplayLogicParams;




GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch {
// class IStream

// namespace StreamPref {

// using namespace prefetch;

// struct IStreamRecStats;
// struct IStreamReplStats;
// struct IStreamMemIFStats;

class IStream;

/******************************************/
/** Base classes of the Stream prefetcher
 * - Buffer entry
 * - Abstract buffer
 * - Memory interface
 */
/******************************************/

/** The BufferEntry ***********
 * Holds all information about a spatial region.
 * Its used for recording as well as replaying,
 */
class BufferEntry : public TaggedEntry{
   private:
    // The parent buffer this entry belongs to.
    // Buffer* parent;
   public:
    Addr _addr;
    uint64_t _mask;
    bool _inst;
    uint8_t _size;
    uint8_t mask_bits;
    uint8_t addr_bits;
    uint8_t total_bits;
    uint64_t history;
    // bool _

    // TODO: This with parameter
    const unsigned MASK_SIZE = 64;
    // const unsigned regionSize;

    // BufferEntry() : _addr(0), _mask(0){};
    // BufferEntry() : BufferEntry(0,0,0);
    BufferEntry(Addr a = 0, uint64_t mask = 0, bool inst = true)
        : _addr(a),
          _mask(mask),
          _inst(inst),
          _size(0),
          mask_bits(0),
          addr_bits(0),
          history(0) {}
    // BufferEntry& operator=(const BufferEntry& other) { return *this; }

    /** Marks a block in the region as touched */
    void touch(uint idx) {
        assert(idx < MASK_SIZE);
        _mask |= (1ULL << idx);
    }

    bool check(uint idx) { return (_mask & (1 << idx)) != 0; }

    bool checkTouch(uint idx) {
        auto r = check(idx);
        touch(idx);
        return r;
    }

    bool empty() { return _mask == 0; }
    // bool check(Addr addr)
    // {
    //   return addr;
    // }

    /** Calculate the number of bits set within a given entry. */
    int bitsSet() {
        int count = 0;
        uint64_t n = _mask;
        while (n != 0) {
            if ((n & 1ULL) == 1ULL) {
                count++;
            }
            n = n >> 1ULL;  // right shift 1 bit
        }
        return count;
    }

    std::string print() {
        std::stringstream ss;
        ss << "A:" << std::hex << _addr;
        ss << ">M:" << std::hex << _mask;
        ss << ">T:" << (_inst ? '1' : '0');
        ss << ">H:" << std::hex << history;
        // } else {
        //   s = "Invalid";
        // }
        return ss.str();
    }

    std::string toReadable() {
        // auto format = "%ull,%ul";
        // auto size = std::snprintf(nullptr, 0, format, _addr,_mask);
        // std::string output(size + 1, '\0');
        // std::sprintf(&output[0], format, _addr,_mask);
        // std::ostring
        return std::to_string(_addr) + "," + std::to_string(_mask) + "," +
               (_inst ? '1' : '0') + "," + std::to_string(history);
    }

    bool parseLine(std::string line) {
        std::stringstream lineStream(line);
        std::string cell;
        if (!std::getline(lineStream, cell, ',')) {
            return false;
        }
        _addr = Addr(std::stoull(cell));
        if (!std::getline(lineStream, cell, ',')) {
            return false;
        }
        _mask = std::stoul(cell);
        if (!std::getline(lineStream, cell, ',')) {
            return false;
        }
        _inst = bool(std::stoi(cell));
        if (!std::getline(lineStream, cell, ',')) {
            return false;
        }
        history = std::stoull(cell);
        return true;
    }

    /** Write the data of the entry to the given memory location
     *
     * @param loc Location to write the data.
     * @return the number of bytes written.
     */
    int writeTo(uint8_t* loc) {
        auto size = sizeof(uint64_t);
        memcpy(loc, (uint8_t*)&_addr, size);
        memcpy(loc + size, (uint8_t*)&_mask, size);
        return 2 * size;
    }
    /** Read the data for this entry from the given memory location
     *
     * @param loc Location to read the data from.
     * @return the number of bytes read.
     */
    int readFrom(uint8_t* loc) {
        auto size = sizeof(uint64_t);
        memcpy((uint8_t*)&_addr, loc, size);
        memcpy((uint8_t*)&_mask, loc + size, size);
        return 2 * size;
    }
    /** Returns how much space in memory the entry requires. */
    int size() { return 2 * sizeof(uint64_t); }

    // bool getValid() { return _valid; }
    // void setValid() { _valid = true; }
};

typedef std::shared_ptr<BufferEntry> BufferEntryPtr;

















/** The Buffer ***********
 * The base class of a buffer to hold spatial region entries
 * It mostly holds the parameters of the spatial regions.
 */
class Buffer {
   public:
    IStream* parent;
    const unsigned bufferEntries;
    const unsigned regionSize;
    unsigned lRegionSize;
    const unsigned blkSize;
    unsigned lBlkSize;
    const unsigned blocksPerRegion;

    // const std::string buffer_name;
    // std::string name() { return buffer_name; }

    // public:
    Buffer(IStream* _parent, unsigned size, unsigned _regionSize,
           unsigned _blkSize)
        : parent(_parent),
          bufferEntries(size),
          regionSize(_regionSize),
          blkSize(_blkSize),
          blocksPerRegion(_regionSize / _blkSize) {
        lRegionSize = floorLog2(regionSize);
        lBlkSize = floorLog2(blkSize);
    }

    /** Calculate the usefullness of a given entry:
     * n block touched in a region / total blocks in one region */
    float usefullness(BufferEntryPtr e) {
        int count = e->bitsSet();
        return (float)count / (float)blocksPerRegion;
    }

    /** Calculate the address for the given entry and index
     * @param e The entry from which we want to calculate the address.
     * @param idx The index in the region.
     */
    Addr calcAddr(BufferEntryPtr e, uint idx) {
        assert(idx < blocksPerRegion);
        return e->_addr + idx * blkSize;
    }
    /**
     * This function will generate all block addresses that where recorded
     * by a given entry to been accessed.
     */
    int genAllAddr(BufferEntryPtr e, std::vector<Addr>& addresses) {
        uint64_t n = e->_mask;
        for (int i = 0; n != 0; i++) {
            if ((n & 1ULL) == 1ULL) {
                addresses.push_back(calcAddr(e, i));
            }
            n = n >> 1ULL;  // right shift 1 bit
        }
        return addresses.size();
    }

    /**
     * This function will generate up to n new address from this entry.
     * For each generated address it will clear the bit in the mask
     *
     * @param e The entry from which we want to calculate the addresses.
     * @param n The number of addresses to be generated.
     * @param addresses The address list where the new entry
     * should be append.
     * @return The number of addresses that where actually generated.
     */
    int genNAddr(BufferEntryPtr e, int n, std::vector<Addr>& addresses) {
        uint64_t i = 0, _n = 0;
        for (; e->_mask != 0 && _n < n; i++) {
            // warn("Entry[%s]. cont.: %i i: %i\n",
            //       e->print(), e->bitsSet(), i, _n);
            if (e->_mask & (1ULL << i)) {
                // warn("Create for i = %i %x\n",i, e->_mask);
                addresses.push_back(calcAddr(e, i));
                _n++;
                e->_mask &= ~(1ULL << i);
                // warn("Create for i = %i %x\n",i, e->_mask);
            }
        }
        return _n;
    }

    void setParent(IStream* _parent) { parent = _parent; }
};

/** The Record Buffer ***********
 * Derived from the base buffer with additional methods necessary for the
 * recording.
 */
class RecordBuffer : public SimObject, public Buffer  {
    friend class IStream;

    // AssociativeSet<BufferEntryPtr> buffer;

    // using HistoryBuffer = CircularQueue<BufferEntry>;
    // HistoryBuffer historyBuffer;






    std::vector<std::deque<BufferEntryPtr>> assocBuffer;

    typedef std::deque<BufferEntryPtr>* BufferPtr;

    std::deque<BufferEntryPtr> _buffer;

    /** Number of entries in the FIFO buffer */

    // std::string

    // std::string name()
    // {
    //   return parent.name() + ".recBuffer";
    // }
    const uint assoc;
    const uint buf_entries;
    uint64_t last_timestamp;


    Addr regionAddress(Addr addr);
    unsigned regionIndex(Addr addr);
    std::pair<Addr, unsigned> regionAddrIdx(Addr addr);

   public:
    // typedef IStreamRecordLogicParams Params;
    // RecordBuffer(const IStreamRecordLogicParams& p,
    //             IStream& _parent, IStreamRecStats& _stats, unsigned size,
    //              unsigned _regionSize = 64, unsigned _blkSize = 64,
    //              std::string _name = ".recBuffer")
    //     : SimObject(p),
    //       Buffer(_parent, size, _regionSize, _blkSize),
    //       stats(_stats),
    //       last_timestamp(0) {}
    RecordBuffer(const IStreamRecordLogicParams& p)
        : SimObject(p),
          Buffer(nullptr, p.buffer_entries, p.region_size, p.block_size),
          assocBuffer(p.assoc), assoc(p.assoc),
          buf_entries(p.buffer_entries / p.assoc),
          last_timestamp(0),
          stats(this)
        {

        }

    ~RecordBuffer() = default;


    uint getIdx(Addr a) {
        return (a / regionSize) % assoc;
    }

    BufferPtr getBuffer(Addr a) {
        return &(assocBuffer[getIdx(a)]);
    }


    void access(Addr addr, uint64_t history_tag = 0);
    /**
     * Add a new entry to the front of the record buffer.
     */
    void add(Addr a) {
        _buffer.push_front(std::make_shared<BufferEntry>(a, 0, true));
    }
    /**
     * This function is to write buffer entries to the trace file in
     * memory until the buffer has again the size it was parameterized.
     * In order to write everything set the all flag when calling.
     *
     * @param all Will write all entries to memory
     */
    void flush(bool all = false);
    void clear() { _buffer.clear(); }
    bool empty() { return _buffer.empty(); }

    protected:

/** * *
 *  Record buffer statistics
 */
struct RecStats : public statistics::Group {
    RecStats(RecordBuffer* parent);

    /** Number of hits and misses in the record buffer */
    statistics::Scalar notifies;
    statistics::Scalar hits;
    statistics::Scalar misses;

    statistics::Scalar cacheHit;
    statistics::Scalar hitL1Cache;
    statistics::Scalar hitL2Cache;
    statistics::Scalar missL1Cache;
    statistics::Scalar missL2Cache;
    statistics::Scalar hitInL1MissQueue;
    statistics::Scalar hitInL2MissQueue;
    statistics::Scalar hitOnPrefetchInL1;
    statistics::Scalar hitOnPrefetchInL2;
    statistics::Scalar instRequest;
    statistics::Scalar readCleanReq;
    statistics::Scalar pfRequest;
    statistics::Scalar accessDrops;

    // /** The hitting entries average distance to the head of
    //  *  the record buffer */
    // statistics::Formula avgHitDistance;

    // Record buffer hits distance distribution
    statistics::Histogram bufferHitDistHist;

    /** Number of writes and drops of entires to the record trace */
    statistics::Scalar entryWrites;
    statistics::Scalar entryDrops;
    statistics::Scalar entryOverflows;

    /** Number of prefetches we recorded in the trace */
    statistics::Scalar totalNumPrefetches;
    statistics::Formula avgNumPrefetchesPerEntry;

    // // Hit stack distance histograms
    // statistics::Histogram hitSDlinHist;
    // statistics::SparseHistogram hitSDlogHist;
} stats;
};





/**
 * The Replay Buffer ***********
 * Derived from the base buffer with additional methods necessary for the
 * replaying a spatial regions.
 */
class ReplayBuffer : public SimObject, public Buffer {
    friend class IStream;
   private:
    std::list<BufferEntryPtr> _buffer;
    // IStreamReplStats& stats;
    /** Number of entries in the FIFO buffer */

    /** Flag in case the end of the trace file was reached. */
    bool eof;
    /** Number of outstanding fill requests to the memory */
    int pendingFills;


   public:
    ReplayBuffer(const IStreamReplayLogicParams& p)
        : SimObject(p),
          Buffer(nullptr, p.buffer_entries, p.region_size, p.block_size),
          eof(false),
          pendingFills(0),
          stats(this) {}
    ~ReplayBuffer() = default;


    // void probe(gem5::Addr addr);
    /**
     * Generate n new addresses that can be filled into the prefetch
     * queue.
     *
     * @param n Number of addresses to be generated.
     */
    std::vector<Addr> generateNAddresses(int n);

    /**
     * Refill the replay buffer upto its maximum capacity by requesting
     * the next records from memory.
     */
    bool refill();

    /**
     * Fill a new entry into the replay buffer.
     *
     * @param entry The entry that should be added.
     */
    void fill(BufferEntryPtr entry);

    bool empty() { return _buffer.empty(); }
    bool traceEnd() { return eof; }

    std::string print() {
        std::string s = "[";
        for (auto it : _buffer) {
            s.append(it->print() + " | ");
        }
        return s + "]";
    }
    void clear() {
        _buffer.clear();
        eof = false;
        pendingFills = 0;
    }

  protected:

    struct ReplStats : public statistics::Group {
    ReplStats(ReplayBuffer* parent);

    /** Number of hits of an notification in the replay buffer */
    statistics::Scalar notifyHits;

    /** Number of hits and misses in the replay buffer */
    statistics::Scalar pfGenerated;
    statistics::Scalar pfInsertedInQueue;
    statistics::Scalar totalPrefetchesInTrace;
    statistics::Scalar pfUseful;
    // statistics::Scalar pfUsefulButData;
    statistics::Scalar pfUsefulButMiss;
    statistics::Scalar demandMshrMisses;

    statistics::Scalar pfBufferHit;
    statistics::Scalar pfHitInCache;
    statistics::Scalar pfTranslationFail;
    // statistics::Scalar translationRetries;
    statistics::Formula pfDrops;

    statistics::Formula accuracy;
    statistics::Formula coverage;

    /** Number of writes and drops of entires to the record trace */
    statistics::Scalar entryReads;
    statistics::Scalar entryDrops;
    // Average usefullness of a entry that falls out of the fifo buffer
    statistics::Formula avgPrefetchesPerEntry;
} stats;
};















/** * *
 *  Memory port interface
 *  The prefetcher uses this interface to read a write the instruction
 *  traces form memory.
 */
class MemoryInterface {
    friend class IStream;
   public:
    /**
     * Helper class that implements the access functionality
     * to the instruction traces in memory
     *
     * @param _port the memory port it belong to
     * @param record_addr_range The address range the record trace should
     *                be placed in memory
     * @param replay_addr_range The address range the replay trace should
     *                be placed in memory
     */
    MemoryInterface(IStream& _parent, AddrRange record_addr_range,
                    AddrRange replay_addr_range, unsigned trace_size,
                    PortProxy& _memProxy,
                    std::string _name = ".replBuffer");
    ~MemoryInterface();

    /**
     * Send one entry to be written into the trace file in memory.
     * The entry enqueued into the write buffer with lower priority
     * than reads.
     *
     * @param entry The entry that should be written to the file.
     * @return true if there is still enough space in the trace
     *         false in case the number of entries reached the maximum.
     */
    bool writeRecord(BufferEntryPtr entry);

    /**
     * Request the next entry from the trace in memory. The
     * interface will fill the data in the replay buffer as soon
     * as the entry is retrived from memory
     * @return true in case there are still entries left. false if
     *          the last entry was already requested. So no entry
     */
    bool readRecord();

    /**
     * Read all entries of the record memory into a provided list
     *
     * @return true if successful. false otherwise.
     */
    bool readRecordMem(std::vector<BufferEntryPtr>& entries);

    bool readCacheLine();
    bool writeCacheLine(std::vector<BufferEntryPtr>& entries);
    void writeCacheLineComplete(Addr addr, Tick wr_start);

    // This reads on cacheline granularity
    bool readRecordMem2(std::vector<BufferEntryPtr>& entries);

    /**
     * Initialize the replay memory a list of entries
     *
     * @param entries list of entries with that the replay memory should
     *          be initialized.
     * @return true if successful. false otherwise.
     */
    bool initReplayMem(std::vector<BufferEntryPtr>& entries);
    // enum ReplayMemState {not_init, init, ready, done};
    // ReplayMemState replayMemState;

    bool replayReady() {
        return (totalReplayEntries > 0) && !replayDone() ? true : false;
    }
    bool replayDone() {
        return (replayIdx >= totalReplayEntries) ? true : false;
    }
    bool recordFull() {
        return recordIdx >= (record_addr_range.size() / entry_size) ? true
                                                                    : false;
    }

   public:
    const std::string _name;
    std::string name() { return _name; }

    void resetRecord() {
        recordIdx = 0;
        recordCLIdx = 0;
        totalRecordEntries = 0;
        effective_bytes_written = 0;
    };
    void resetReplay() { replayIdx = 0; };

    DrainState drain();

   private:
    /** The parent I Stream prefetcher */
    IStream& parent;

    /** The proxy the memory for reading and initializing the record
     * and replay memory.
     */
    PortProxy* memProxy;

    /** The address range where the record trace should be located. */
    AddrRange record_addr_range;
    /** The address range where the replay trace should be located. */
    AddrRange replay_addr_range;

    /** Index to write the next record entry */
    unsigned int recordIdx;
    unsigned int recordCLIdx;
    /** Number of record entries in memory */
    unsigned int totalRecordEntries;
    unsigned int totalRecordCLs;
    /** A small buffer to fill a entire cache line before writing. */
    std::vector<BufferEntryPtr> wrEntries;

    /** Actual index of the next replay entry */
    unsigned int replayIdx;
    /** Number of replay entries in memory */
    unsigned int totalReplayEntries;
    unsigned int totalReplayCLs;

    /** A small buffer read an entire cache line at once. */
    std::vector<BufferEntryPtr> rdEntries;

    /** Size one buffer entry requires to store in memory */
    unsigned int entry_size;
    const unsigned int blk_size;
    const unsigned int region_size;
    const unsigned int history_bits;

    /** Real entry size in bytes */
    // unsigned realEntrySize;
    unsigned effectiveEntrySize;
    unsigned entrySize_n_mask;
    unsigned entrySize_n_addr;
    unsigned entrySize_n_hist;
    unsigned entries_per_cl;

    /** Two additional variables as we only model the  */
    unsigned int effective_bytes_written;
    const unsigned int trace_size;

    void writeRecordComplete(Addr addr, Tick wr_start);
    void readRecordComplete(Addr addr, uint8_t* data, Tick rd_start);

    /** Serialize an buffer entry to byte array which can be written to
     * memory.
     *
     * @param entry The buffer entry to be serialized
     * @param data* A byte array pointer where the data will be written
     *              The pointer needs to be null as the function will
     *              allocate the neccessary memory
     * @return Number of bytes that has been serialized.
     */
    int serialize(BufferEntryPtr entry, uint8_t* data) {
        // We have 48 bit addresses. Therefore we first mask the 16 MSBs.
        // Then we cut of the unnecessary LSBs and finaly we copy
        // the remaining bytes to the data buffer.
        int64_t tmp = entry->_addr << (64 - 48);
        int64_t tmp2 = tmp >> ((64 - 48) + floorLog2(region_size));
        DPRINTF(HWPrefetch, "%x | %x \n", tmp, tmp2);
        memcpy(data, (uint8_t*)&tmp2, entrySize_n_addr);
        memcpy(data + entrySize_n_addr, (uint8_t*)&entry->_mask,
               entrySize_n_mask);

        entry->history =
            (entry->history & ((1ULL << (history_bits - 1)) - 1)) |
            ((entry->_inst ? 1ULL : 0ULL) << (history_bits - 1));
        memcpy(data + entrySize_n_addr + entrySize_n_mask,
               (uint8_t*)&entry->history, entrySize_n_hist);
        // The upper most bit of the history is used to store the type of
        // the entry.
        DDUMP(HWPrefetch, data, entry_size);
        return entry_size;
    }

    /** Deserialize an buffer entry from a byte array (which was read from
     * memory.)
     *
     * @param entry The buffer entry be serialized
     * @param data* A byte array pointer where the data should be written
     * @return Number of bytes that has been serialized.
     */
    int deserialize(BufferEntryPtr entry, uint8_t* data) {
        uint64_t tmp = 0;
        memcpy((uint8_t*)&tmp, data, entrySize_n_addr);

        int64_t tmp2 = tmp << ((64 - 48) + floorLog2(region_size));
        int64_t tmp3 = tmp2 >> (64 - 48);
        DPRINTF(HWPrefetch, "%x | %x | %x \n", tmp, tmp2, tmp3);
        entry->_addr = tmp3;

        memcpy((uint8_t*)&entry->_mask, data + entrySize_n_addr,
               entrySize_n_mask);
        memcpy((uint8_t*)&entry->history,
               data + entrySize_n_addr + entrySize_n_mask,
               entrySize_n_hist);
        // The upper most bit of the history is used to store the type of
        // the entry.
        entry->_inst =
            (entry->history & (1ULL << (history_bits - 1))) ? true : false;
        DDUMP(HWPrefetch, data, entry_size);
        return entry_size;
    }

    /** Serialize an buffer entry to byte array which can be written to
     * memory.
     *
     * @param entry The buffer entry to be serialized
     * @return A pointer to the serialized data
     */
    uint8_t* serialize(BufferEntryPtr entry) {
        uint8_t* data = new uint8_t[entry_size];
        serialize(entry, data);
        return data;
    }

    /** Deserialize an buffer entry from a byte array (which was read from
     * memory.)
     *
     * @param data* A byte array pointer from where the data
     *              for the new entry will be read.
     * @return The desialized buffer entry.
     */
    BufferEntryPtr deserialize(uint8_t* data) {
        BufferEntryPtr entry = std::make_shared<BufferEntry>();
        deserialize(entry, data);
        return entry;
    }

  protected:

  struct MemIFStats : public statistics::Group {
      MemIFStats(IStream* parent, const std::string& name);

      /** Count the number of generated packets. */
      statistics::Scalar numPackets;
      /** Count the number of retries. */
      statistics::Scalar numRetries;

      /** Count the time incurred from back-pressure. */
      statistics::Scalar retryTicks;

      /** Count the number of reads/writes read. */
      statistics::Scalar bytesRead;
      statistics::Scalar bytesWritten;
      statistics::Scalar effBytesWritten;

      /** Total num of ticks read reqs took to complete  */
      statistics::Scalar totalReadLatency;
      statistics::Scalar totalWriteLatency;
      /** Count the number reads/writes */
      statistics::Scalar totalReads;
      statistics::Scalar totalWrites;
      /** Avg num of ticks each read/write req took to complete  */
      statistics::Formula avgReadLatency;
      statistics::Formula avgWriteLatency;
      /** read/write bandwidth in bytes/s  */
      statistics::Formula readBW;
      statistics::Formula writeBW;
  } stats;
};




/*******************************************************************
 *
 *  The Main Stream prefetcher.
 * Wraps around other classes and coordinates communication
 *
 *******************************************************************/

class IStream : public Queued {
    // struct IStreamRecStats;
    // struct IStreamReplStats;

    friend class RecordBuffer;
    friend class ReplayBuffer;
    friend class MemoryInterface;
    friend struct IStreamReplStats;

   public:
    typedef IStreamPrefetcherParams Params;
    IStream(const Params& p);
    ~IStream() = default;

    void init() override;

    DrainState drain() override;

   private:
    /** Buffer to track accesses
     *
     */

   protected:
    Port& getPort(const std::string& if_name,
                  PortID idx = InvalidPortID) override;

    // Main memory port
    class IStreamMemPort : public DmaPort {
       public:
        IStreamMemPort(IStream* _parent, System* sys)
            : DmaPort(_parent, sys), parent(*_parent) {}

       private:
        IStream& parent;
    };

    // Addr regionAddress(Addr addr);
    // unsigned regionIndex(Addr addr);
    // std::pair<Addr, unsigned> regionAddrIdx(Addr addr);

    void replay(std::vector<Addr>& addresses);

    std::vector<BufferEntry> buffer;

   protected:
    // PARAMETERS
    /** Pointr to the parent components. */
    MemoryInterface* memInterface;
    IStreamMemPort port;
    // IStreamMemBDPort backdoor;
    // IStreamMemdmaPort dmaPort;
    RecordBuffer* recordBuffer;
    ReplayBuffer* replayBuffer;

    /**
     * Switches to enable/disable the two distinct
     * features of the IStream prefetcher
     */
    bool enable_record;
    bool enable_replay;

    const bool startOnCS;
    const bool stopOnCS;
    bool waitForCSActive;
    bool waitForCSIdle;

    /** Record Hits in the target cache */
    const bool recordHitInTargetCache;
    /** Record Hits in the listener cache */
    const bool recordHitInListCache;
    /** Record Hits of prefetches in the target cache */
    const bool recordHitOnPfTargetCache;
    /** Record Hits of prefetches in the listener cache */
    const bool recordHitOnPfListCache;
    /** Record N for a miss */
    const unsigned recNAtMiss;

    /** The size of one spatial region. */
    const unsigned regionSize;
    /** log_2(block size spatil region). */
    unsigned lRegionSize;
    /** Number of blocks that fit in one region */
    unsigned blksPerRegion;

    /** Number bits used for the recording history*/
    const unsigned historyBits;

    // /** Real entry size in bytes */
    // unsigned realEntrySize;
    // unsigned effectiveEntrySize;
    // unsigned entrySize_n_mask;
    // unsigned entrySize_n_addr;
    // unsigned entrySize_n_hist;
    // unsigned entries_per_cl;

    /** Number number of prefetches have already recorded and replayed. */
    unsigned pfRecorded;
    unsigned pfReplayed;
    /** Total number number of prefetches the trace contains. */
    unsigned totalPrefetches;
    /** Counter to monitor how many of the issued prefetches we actually
     * used. This counter is used to steer the replay run ahead.
     */
    unsigned pfIssued, pfUsed, pfUnused;

    /** Replay run ahead distance */
    const signed replayDistance;

    /** Stack distance of meta data */
    StackDistCalc sdcalc;

    // /**
    //  * Callback to flush and close all open output streams on exit. If
    //  * we were calling the destructor it could be done there.
    //  */
    // void closeStreams();
    std::string readFileIntoString(const std::string& path);
    /**
     * Function to calculate the stack distance statistics for the record
     * accesses.
     */
    void calcStackDistance(Addr addr);

    // class CacheListener : public ProbeListenerArgBase<PacketPtr> {
    //    public:
    //     CacheListener(IStream& _parent, ProbeManager* pm,
    //                   const std::string& name, bool _l1cache = false,
    //                   bool _miss = false)
    //         : ProbeListenerArgBase(pm, name),
    //           parent(_parent),
    //           isLevel1(_l1cache),
    //           miss(_miss) {}
    //     void notify(const PacketPtr& pkt) override;

    //    protected:
    //     IStream& parent;
    //     const bool isLevel1;
    //     const bool miss;
    // };

    // std::vector<CacheListener*> cacheListeners;
    std::vector<ProbeListener *> listeners;

    /** The two caches for the I-Stream prefetcher */
    BaseCache* l1Cache;
    BaseCache* l2Cache;

    SimObject* contextSwitchHook;

   public:
    /** Registered tlb for address translations */
    BaseCache* listenerCache;

    /** Registered tlb for address translations */
    void addListenerCache(BaseCache* cache) { listenerCache = cache; }

    /** Handler to register the caches. */
    void addCaches(BaseCache* _l1cache, BaseCache* _l2cache) {
        l1Cache = _l1cache;
        l2Cache = _l2cache;
    }

    /**
     * Register probe points for this object.
     */
    void regProbeListeners() override;
    bool checkPacket(const PacketPtr& pkt);

    enum AccessType { INV, HIT_L1, HIT_L1_PF, HIT_L2, HIT_L2_PF, MISS_L2 };
    AccessType observeAccess(const PacketPtr& pkt, bool miss);

    void record(const Addr addr, AccessType atype);

    bool filterAccessTypes(AccessType accType);
    void probeNotify(const PacketPtr& pkt, bool miss) override;
    void notifyRecord(const PacketPtr& pkt, bool miss);
    void notifyReplay(const PacketPtr& pkt, bool miss);

    /**
     * Methods when the cache notifies the prefetcher.
     */
    void notifyFromLevel1(const PacketPtr& pkt, bool miss);
    void notifyFromLevel2(const PacketPtr& pkt, bool miss);

    void notify(const PacketPtr& pkt, const PrefetchInfo& pfi);

    void calculatePrefetch(const PrefetchInfo& pfi,
                           std::vector<AddrPriority>& addresses) {};

    /**
     * Method to insert create a new prefetch request.
     */
    void insert(const PacketPtr& pkt, PrefetchInfo& new_pfi, int32_t priority);

    /**
     * Calculate the target physical address (PA).
     */
    void proceedWithTranslation(const PacketPtr& pkt, PrefetchInfo& new_pfi,
                                int32_t priority);

    /**
     * Indicates that the translation of the address of the provided
     * deferred packet has been successfully completed, and it can be
     * enqueued as a new prefetch request.
     * In case the translation fails it will remain in the queue and the
     * translation will be retried later again.
     * @param dp the deferred packet that has completed the translation req.
     * @param failed whether the translation was successful
     */
    void translationComplete(DeferredPacket* dp, bool failed) override;

    void squashPrefetches(Addr addr, bool is_secure);

    void prefetchUnused() override {
        prefetchStats.pfUnused++;
        pfUnused++;
    }
    PacketPtr getPacket() override {
        PacketPtr pkt = Queued::getPacket();
        if (pkt != nullptr) {
            pfIssued++;
        }
        return pkt;
    }

    void startRecord(bool wait_for_cs = false);
    bool startReplay(bool wait_for_cs = false);
    void stopRecord() {
        recordBuffer->flush(true);
        enable_record = false;
        enable_replay = false;
    }
    bool stopReplay() { enable_replay = false; }
    void startAtScheduling();

   public:
    void initReplay(std::string filename);

   public:
    void dumpRecTrace(std::string filename);

   protected:
    // IStreamRecStats recordStats;
    // IStreamReplStats replayStats;
    // IStreamMemIFStats memIFStats;
};

// }  // namespace StreamPref
}  // namespace prefetch
}  // namespace gem5

#endif  // __MEM_CACHE_PREFETCH_ISTREAM_HH__
