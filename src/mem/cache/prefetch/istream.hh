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
#include "mem/packet.hh"
// #include "mem/port.hh"
#include "mem/request.hh"
#include "mem/stack_dist_calc.hh"

namespace gem5
{

  struct IStreamPrefetcherParams;

  GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
  namespace prefetch
  {

    class IStream : public Queued
    {
        /**
         * Probe Listener to handle probe events probe events from context
         * switches
         */
        class PrefetchListenerCS : public ProbeListenerArgBase<bool>
        {
          public:
            PrefetchListenerCS(IStream &_parent, ProbeManager *pm,
                             const std::string &name)
                : ProbeListenerArgBase(pm, name),
                  parent(_parent) {}
            void notify(const bool &active) override;
          protected:
            IStream &parent;
            std::string name() { return parent.name() + ".cs"; }
        };

        /** Array of probe listeners */
        std::vector<PrefetchListenerCS *> listenersSC;


    public:

        /**
         * Add a SimObject and a probe name to monitor context switches
         * @param obj The SimObject pointer to listen from
         * @param name The probe name
         */
        void addEventProbeCS(SimObject *obj, const char *name);
        void addCSHook(SimObject * obj) {
            contextSwitchHook = obj;
        }


      public:
        IStream(const IStreamPrefetcherParams& p);
        ~IStream() = default;

        void init() override;

        DrainState drain() override;

      private:
      /** Buffer to track accesses
       *
      */


      class Buffer;

      struct BufferEntry
      {
        private:
        // The parent buffer this entry belongs to.
        // Buffer* parent;
        public:
        Addr _addr;
        uint64_t _mask;


        // TODO: This with parameter
        const unsigned MASK_SIZE = 64;
        // const unsigned regionSize;


        // BufferEntry() : _addr(0), _mask(0){};
        BufferEntry(Addr a = 0, uint64_t mask = 0)
          : _addr(a), _mask(mask)
        {
        }
        // BufferEntry& operator=(const BufferEntry& other) { return *this; }

        /** Marks a block in the region as touched */
        void touch(uint idx)
        {
          assert(idx < MASK_SIZE);
          _mask |= (1ULL << idx);
        }

        bool check(uint idx)
        {
          return (_mask & (1 << idx)) != 0;
        }

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
        int bitsSet()
        {
          int count = 0;
          uint64_t n = _mask;
          while (n != 0) {
            if ((n & 1ULL) == 1ULL) {
              count++;
            }
            n = n >> 1ULL; //right shift 1 bit
          }
          return count;
        }

//         float usefullness()
//         {
//           int count = 0;
//           unsigned long n = _mask;
//           while (n != 0) {
//             if ((n & 1) == 1) {
//               count++;
//             }
//             n = n >> 1; //right shift 1 bit
//           }
//           return (float)count;
//           // / (float)parent->regionSize;
//         }

//         /** Calculate the address for a given index */
//         Addr calcAddr(uint idx)
//         {
//           // assert(idx < parent->regionSize);





// /*

// asdfasdfa
// sd
// asd
// fasdfasdf
// asd
// fasdfasd
// f
// asd
// fa
// dsf
// ads
// f
// adsf
// a
// sd



// */




//           // return _addr + idx * parent->blkSize;
//           return _addr + idx * 64;
//         }
//         /**
//          * This function will generate all block addresses that where
//          * recorded to been accessed in this region.
//         */
//         int genAllAddr(std::vector<Addr>& addresses)
//         {
//           unsigned long n = _mask;
//           for (int i = 0; n != 0; i++) {
//             if ((n & 1) == 1) {
//               addresses.push_back(calcAddr(i));
//             }
//             n = n >> 1; //right shift 1 bit
//           }
//           return addresses.size();
//         }

//         /**
//          * This function will generate up to n new address from this entry.
//          * For each generated address it will clear the bit in the mask
//          *
//          * @param n The number of addresses to be generated.
//          * @param addresses The address list where the new entry
//          * should be append.
//           * @return The number of addresses that where actually generated.
//           */
//         int genNAddr(int n, std::vector<Addr>& addresses)
//         {
//           int i = 0, _n = 0;
//           for (; _mask != 0 && _n < n; i++) {
//             if (_mask & (1<<i)) {
//               addresses.push_back(calcAddr(i));
//               _n++;
//               _mask &= ~(1<<i);
//             }
//           }
//           return _n;
//         }

        // /**
        //  * This function will generate up to n addresses
        //  * from this entry. It will mark the entries as
        // */
        // int genNAddr(int n, std::vector<Addr>& addresses)
        // {
        //   unsigned long n = _mask;
        //   for (int i = 0; n != 0; i++) {
        //     if ((n & 1) == 1) {
        //       addresses.push_back(calcAddr(i));
        //     }
        //     n = n >> 1; //right shift 1 bit
        //   }
        //   return addresses.size();
        // }

        std::string print() {
          std::stringstream ss;
          ss << "A:" << std::hex << _addr;
          ss << ">M:" << std::hex << _mask;
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
          return std::to_string(_addr) + "," + std::to_string(_mask);
        }
        bool parseLine(std::string line) {
          std::stringstream lineStream(line);
          std::string cell;
          if (!std::getline(lineStream,cell, ',')) {
            return false;
          }
          _addr = Addr(std::stoull(cell));
          if (!std::getline(lineStream,cell, ',')) {
            return false;
          }
          _mask = std::stoul(cell);
          return true;
        }

        /** Write the data of the entry to the given memory location
         *
         * @param loc Location to write the data.
         * @return the number of bytes written.
         */
        int writeTo(uint8_t* loc) {
          auto size = sizeof(uint64_t);
          memcpy(loc, (uint8_t*)&_addr , size);
          memcpy(loc + size, (uint8_t*)&_mask , size);
          return 2*size;
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
          return 2*size;
        }
        /** Returns how much space in memory the entry requires. */
        int size() { return 2*sizeof(uint64_t); }

        // bool getValid() { return _valid; }
        // void setValid() { _valid = true; }


      };
      typedef std::shared_ptr<BufferEntry> BufferEntryPtr;

      class Buffer
      {
        public:
        IStream& parent;
        const unsigned bufferEntries;
        const unsigned regionSize;
        unsigned lRegionSize;
        const unsigned blkSize;
        const unsigned blocksPerRegion;


        // public:
          Buffer(IStream& _parent, unsigned size,
            unsigned _regionSize, unsigned _blkSize)
            :  parent(_parent),
              bufferEntries(size),
              regionSize(_regionSize),
              blkSize(_blkSize),
              blocksPerRegion(_regionSize/_blkSize) {
            lRegionSize = floorLog2(regionSize);
          }


        /** Calculate the usefullness of a given entry:
         * n block touched in a region / total blocks in one region */
        float usefullness(BufferEntryPtr e)
        {
          int count = e->bitsSet();
          return (float)count / (float)blocksPerRegion;
        }

        /** Calculate the address for the given entry and index
         *
         * @param e The entry from which we want to calculate the address.
         * @param idx The index in the region.
         */
        Addr calcAddr(BufferEntryPtr e, uint idx)
        {
          assert(idx < blocksPerRegion);
          return e->_addr + idx * blkSize;
        }
        /**
         * This function will generate all block addresses that where recorded
         * by a given entry to been accessed.
        */
        int genAllAddr(BufferEntryPtr e, std::vector<Addr>& addresses)
        {
          uint64_t n = e->_mask;
          for (int i = 0; n != 0; i++) {
            if ((n & 1ULL) == 1ULL) {
              addresses.push_back(calcAddr(e,i));
            }
            n = n >> 1ULL; //right shift 1 bit
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
        int genNAddr(BufferEntryPtr e, int n, std::vector<Addr>& addresses)
        {
          uint64_t i = 0, _n = 0;
          for (; e->_mask != 0 && _n < n; i++) {
            // warn("Entry[%s]. cont.: %i i: %i\n",
            //       e->print(), e->bitsSet(), i, _n);
            if (e->_mask & (1ULL<<i)) {
              // warn("Create for i = %i %x\n",i, e->_mask);
              addresses.push_back(calcAddr(e,i));
              _n++;
              e->_mask &= ~(1ULL << i);
              // warn("Create for i = %i %x\n",i, e->_mask);
            }
          }
          return _n;
        }

      };















      class RecordBuffer : public Buffer
      {
        std::deque<BufferEntryPtr> _buffer;
        /** Number of entries in the FIFO buffer */

        std::string name()
        {
          return parent.name() + ".recBuffer";
        }

      public:
        RecordBuffer(IStream& _parent, unsigned size,
          unsigned _regionSize = 64, unsigned _blkSize = 64)
          : Buffer(_parent, size, _regionSize, _blkSize)
        {
        }

        void access(Addr addr);
        /**
         * Add a new entry to the front of the record buffer.
         */
        void add(Addr a) {
          _buffer.push_front(std::make_shared<BufferEntry>(a));
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
      };

      class ReplayBuffer : public Buffer
      {
      private:
        std::list<BufferEntryPtr> _buffer;
        /** Number of entries in the FIFO buffer */

        /** Flag in case the end of the trace file was reached. */
        bool eof;
        /** Number of outsanding fill requests to the memory */
        int pendingFills;

        std::string name() {
          return parent.name() + ".replBuffer";
        }

      public:
        ReplayBuffer(IStream& _parent, unsigned size,
          unsigned _regionSize = 64, unsigned _blkSize = 64)
          : Buffer(_parent, size, _regionSize, _blkSize),
            eof(false), pendingFills(0)
        {
        }

        void probe(gem5::Addr addr);
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
      };






      // class TraceStream
      // {
      // public:

      //   /**
      //    * Create an output stream for a given file name.
      //    * @param filename Path to the file to create or truncate
      //    */
      //   TraceStream(const std::string& filename);
      //   ~TraceStream();

      //   /**
      //    * Read or write a buffer entry to the trace stream.
      //    *
      //    * @param entry The entry that should be written to the file.
      //    */
      //   void write(BufferEntry* entry);
      //   bool read(BufferEntry* entry);

      //   std::string name() {
      //     return ".tracestream";
      //   }

      //   void reset();

      // private:
      //   /// Underlying file output stream
      //   std::fstream fileStream;
      // };


      /** * *
       *  Memory port interface
       *  The prefetcher uses this interface to read a write the instruction
       *  traces form memory.
       */
      class MemoryInterface
      {
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
        MemoryInterface(IStream& _parent,
                        AddrRange record_addr_range,
                        AddrRange replay_addr_range,
                        PortProxy &_memProxy);
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
          return recordIdx >= (record_addr_range.size() / entry_size)
                ? true : false;
        }






      public:
        std::string name() {
          return parent.name() + ".memInterface";
        }


        void resetRecord() {
          recordIdx = 0;
          totalRecordEntries = 0;
        };
        void resetReplay() {
          replayIdx = 0;
        };

        DrainState drain();

      private:

        /** The parent I Stream prefetcher */
        IStream& parent;

        /** The proxy the memory for reading and initializing the record
         * and replay memory.
         */
        PortProxy *memProxy;

        /** The address range where the record trace should be located. */
        AddrRange record_addr_range;
        /** The address range where the replay trace should be located. */
        AddrRange replay_addr_range;

        /** Index to write the next record entry */
        unsigned int recordIdx;
        /** Number of record entries in memory */
        unsigned int totalRecordEntries;

        /** Actual index of the next replay entry */
        unsigned int replayIdx;
        /** Number of replay entries in memory */
        unsigned int totalReplayEntries;

        /** Size one buffer entry requires to store in memory */
        unsigned int entry_size;

        // /** Packets waiting to be sent. */
        // std::list<PacketPtr> blockedPkts;
        // // PacketPtr retryPkt;

        // /** Tick when the stalled packet was meant to be sent. */
        // Tick retryPktTick;

        // /** Set when we blocked waiting for outstanding reqs */
        // bool blockedWaitingResp;

        // const int maxOutstandingReqs;

        // /** Reqs waiting for response **/
        // std::unordered_map<RequestPtr,Tick> waitingResp;

        // /**
        //  * Puts this packet in the waitingResp list and returns true if
        //  * we are above the maximum number of oustanding requests.
        //  */
        // bool allocateWaitingRespSlot(PacketPtr pkt);

      // /**
      //  * Generate a new request and associated packet
      //  *
      //  * @param addr Physical address to use
      //  * @param size Size of the request
      //  * @param cmd Memory command to send
      //  * @param flags Optional request flags
      //  */
      // PacketPtr getPacket(Addr addr, unsigned size, const MemCmd& cmd,
      //                     Request::FlagsType flags = 0);
      // /**
      //  * Generate a new request and associated packet
      //  *
      //  * @param pkt The packet to be send.
      //  * @param atomic The protocol used for sending. Default is timining.
      //  */
      // bool sendPacket(PacketPtr pkt, bool atomic=false);

        // void readResp(BufferEntry);
        // // GenericCache::ExternalInterface
        // // Cache will call these methods to read data from
        //       external (main memory)
        // // to the cache and write data the other way round
        // void read(Addr addr, uint8_t *data, uint64_t size,
        //           std::function<void(void)> callback);

        // void write(Addr addr, uint8_t *data, uint64_t size,
        //        std::function<void(void)> callback);



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
          int64_t tmp = entry->_addr << (64-48);
          int64_t tmp2 = tmp >> ((64-48) + parent.lRegionSize);
          DPRINTF(HWPrefetch, "%x | %x \n", tmp, tmp2);
          memcpy(data, (uint8_t*)&tmp2, parent.entrySize_n_addr);
          memcpy(data + parent.entrySize_n_addr,
                (uint8_t*)&entry->_mask, parent.entrySize_n_mask);
          DDUMP(HWPrefetch, data, parent.realEntrySize);
          return parent.realEntrySize;
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
          memcpy((uint8_t*)&tmp, data, parent.entrySize_n_addr);

          int64_t tmp2 = tmp << ((64-48) + parent.lRegionSize);
          int64_t tmp3 = tmp2 >> (64-48);
          DPRINTF(HWPrefetch, "%x | %x | %x \n", tmp, tmp2,tmp3);
          entry->_addr = tmp3;

          memcpy((uint8_t*)&entry->_mask,
                  data + parent.entrySize_n_addr, parent.entrySize_n_mask);
          DDUMP(HWPrefetch, data, parent.realEntrySize);
          return parent.realEntrySize;
        }

        /** Serialize an buffer entry to byte array which can be written to
         * memory.
         *
         * @param entry The buffer entry to be serialized
         * @return A pointer to the serialized data
        */
        uint8_t* serialize(BufferEntryPtr entry) {
          uint8_t* data = new uint8_t[parent.realEntrySize];
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
        /** Read */
        // void fetchDescriptor(Addr address);
        // void fetchDescComplete();
        // EventFunctionWrapper fetchCompleteEvent;



        // void initMem(Addr addr, uint8_t *data, uint64_t size);
        // void initMemComplete(uint8_t *data);
        // // EventFunctionWrapper initMemCompleteEvent;

        // void readMem(Addr addr, uint8_t *data, uint64_t size);
        // void readMemComplete();
        // EventFunctionWrapper readMemCompleteEvent;





        public:
        /**
        //  * Receive a retry from the neighbouring port and attempt to
        //  * resend the waiting packet.
        //  */
        // void recvReqRetry();
        // void retryReq();

        // /**
        //  * Callback to receive responses for outstanding memory requests.
        //  */
        // bool recvTimingResp(PacketPtr pkt);

      };

    protected:
      Port &getPort(const std::string &if_name,
                    PortID idx=InvalidPortID) override;
      // bool trySatisfyFunctional(PacketPtr pkt);


      // /** Request port specialisation for the Stream prefetcher */
      // class IStreamMemPort : public RequestPort
      // {
      //   public:

      //     IStreamMemPort(const std::string& name, IStream& _parent);

      //   protected:

      //     void recvReqRetry() { parent.memInterface->retryReq(); }

      //     bool recvTimingResp(PacketPtr pkt)
      //     { return parent.memInterface->recvTimingResp(pkt); }

      //     // void recvTimingSnoopReq(PacketPtr pkt) { }
      //     // void recvFunctionalSnoop(PacketPtr pkt) { }
      //     // Tick recvAtomicSnoop(PacketPtr pkt) { return 0; }

      //   private:
      //     IStream& parent;
      // };

      // /** Request port specialisation for the Stream prefetcher */
      // class IStreamMemBDPort : public RequestPort
      // {
      //   public:
      //     IStreamMemBDPort(const std::string& name, IStream& _parent);

      //   protected:
      //     void recvReqRetry() {  }
      //     bool recvTimingResp(PacketPtr pkt) { return false; }
      //   private:
      //     IStream& parent;
      // };






// Main memory port
    class IStreamMemPort : public DmaPort
    {
      public:
        IStreamMemPort(IStream *_parent, System *sys)
            : DmaPort(_parent, sys), parent(*_parent) {}
      private:
        IStream& parent;
    };


















      Addr regionAddress(Addr addr);
      unsigned regionIndex(Addr addr);
      std::pair<Addr, unsigned> regionAddrIdx(Addr addr);

       void replay(std::vector<Addr> &addresses);

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
      /** Real entry size in bytes */
      unsigned realEntrySize;
      unsigned entrySize_n_mask;
      unsigned entrySize_n_addr;

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



        class CacheListener : public ProbeListenerArgBase<PacketPtr>
        {
        public:
            CacheListener(IStream &_parent, ProbeManager *pm,
                            const std::string &name, bool _l1cache = false,
                            bool _miss = false)
                : ProbeListenerArgBase(pm, name),
                parent(_parent), isLevel1(_l1cache), miss(_miss) {}
            void notify(const PacketPtr &pkt) override;
        protected:
            IStream &parent;
            const bool isLevel1;
            const bool miss;
        };

        std::vector<CacheListener *> cacheListeners;

        /** The two caches for the I-Stream prefetcher */
        BaseCache * l1Cache;
        BaseCache * l2Cache;


        SimObject * contextSwitchHook;



    public:
/** Registered tlb for address translations */
      BaseCache * listenerCache;

/** Registered tlb for address translations */
      void addListenerCache(BaseCache * cache) {
        listenerCache = cache;
      }



      /** Handler to register the caches. */
      void addCaches(BaseCache * _l1cache, BaseCache * _l2cache) {
        l1Cache = _l1cache;
        l2Cache = _l2cache;
      }




    /**
     * Register probe points for this object.
     */
    void regProbeListeners() override;
        bool checkPacket(const PacketPtr &pkt);

      enum AccessType
      {
          INV,HIT_L1,HIT_L1_PF,
          HIT_L2, HIT_L2_PF, MISS_L2
      };
      AccessType observeAccess(const PacketPtr &pkt, bool miss);

      void record(const Addr addr, AccessType atype);

      bool filterAccessTypes(AccessType accType);
      void probeNotify(const PacketPtr &pkt, bool miss) override;
      void notifyRecord(const PacketPtr &pkt, bool miss);
      void notifyReplay(const PacketPtr &pkt, bool miss);

        /**
         * Methods when the cache notifies the prefetcher.
         */
        void notifyFromLevel1(const PacketPtr &pkt, bool miss);
        void notifyFromLevel2(const PacketPtr &pkt, bool miss);

      void notify(const PacketPtr &pkt, const PrefetchInfo &pfi);


      void calculatePrefetch(const PrefetchInfo& pfi,
        std::vector<AddrPriority>& addresses);

      /**
       * Method to insert create a new prefetch request.
       */
      void insert(const PacketPtr &pkt,
        PrefetchInfo &new_pfi, int32_t priority);

      /**
       * Calculate the target physical address (PA).
       */
      void proceedWithTranslation(const PacketPtr &pkt,
        PrefetchInfo &new_pfi, int32_t priority);

      /**
       * Indicates that the translation of the address of the provided
       * deferred packet has been successfully completed, and it can be
       * enqueued as a new prefetch request.
       * In case the translation fails it will remain in the queue and the
       * translation will be retried later again.
       * @param dp the deferred packet that has completed the translation req.
       * @param failed whether the translation was successful
       */
      void translationComplete(DeferredPacket *dp, bool failed) override;

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
      bool stopReplay() {
        enable_replay = false;
      }
      void startAtScheduling();

    public:
      void initReplay(std::string filename);

    public:
      void dumpRecTrace(std::string filename);


















    protected:
      struct IStreamRecStats : public statistics::Group
      {
        IStreamRecStats(IStream* parent, const std::string& name);

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

        // Hit stack distance histograms
        statistics::Histogram hitSDlinHist;
        statistics::SparseHistogram hitSDlogHist;

      } recordStats;

      struct IStreamReplStats : public statistics::Group
      {
        IStreamReplStats(IStream* parent, const std::string& name);

        /** Number of hits of an notification in the replay buffer */
        statistics::Scalar notifyHits;

        /** Number of hits and misses in the replay buffer */
        statistics::Scalar pfGenerated;
        statistics::Scalar pfInsertedInQueue;
        statistics::Scalar totalPrefetchesInTrace;
        statistics::Scalar pfUseful;
        // statistics::Scalar pfUsefulButData;
        statistics::Scalar pfUsefulButMiss;

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

      } replayStats;


      struct IStreamMemIFStats : public statistics::Group
      {
        IStreamMemIFStats(IStream* parent, const std::string& name);

        /** Count the number of generated packets. */
        statistics::Scalar numPackets;

        /** Count the number of retries. */
        statistics::Scalar numRetries;

        /** Count the time incurred from back-pressure. */
        statistics::Scalar retryTicks;

        /** Count the number of bytes read. */
        statistics::Scalar bytesRead;

        /** Count the number of bytes written. */
        statistics::Scalar bytesWritten;

        /** Total num of ticks read reqs took to complete  */
        statistics::Scalar totalReadLatency;

        /** Total num of ticks write reqs took to complete  */
        statistics::Scalar totalWriteLatency;

        /** Count the number reads. */
        statistics::Scalar totalReads;

        /** Count the number writes. */
        statistics::Scalar totalWrites;

        /** Avg num of ticks each read req took to complete  */
        statistics::Formula avgReadLatency;

        /** Avg num of ticks each write reqs took to complete  */
        statistics::Formula avgWriteLatency;

        /** Read bandwidth in bytes/s  */
        statistics::Formula readBW;

        /** Write bandwidth in bytes/s  */
        statistics::Formula writeBW;
      } memIFStats;


    };

  } // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_ISTREAM_HH__
