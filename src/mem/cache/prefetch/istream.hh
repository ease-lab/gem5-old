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
  * Describes a IStream prefetcher.
  */

#ifndef __MEM_CACHE_PREFETCH_ISTREAM_HH__
#define __MEM_CACHE_PREFETCH_ISTREAM_HH__

#include <iostream>
#include <vector>

#include "debug/HWPrefetch.hh"
#include "dev/dma_device.hh"
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
        unsigned long _mask;


        // TODO: This with parameter
        const unsigned MASK_SIZE = 64;
        // const unsigned regionSize;


        // BufferEntry() : _addr(0), _mask(0){};
        BufferEntry(Addr a = 0, int mask = 0)
          : _addr(a), _mask(mask)
        {
        }
        // BufferEntry& operator=(const BufferEntry& other) { return *this; }

        /** Marks a block in the region as touched */
        void touch(uint idx)
        {
          assert(idx < MASK_SIZE);
          _mask |= (1 << idx);
        }

        bool check(uint idx)
        {
          return (_mask & (1 << idx)) != 0;
        }

        bool empty() { return _mask == 0; }
        // bool check(Addr addr)
        // {
        //   return addr;
        // }

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
          int count = 0;
          unsigned long n = e->_mask;
          while (n != 0) {
            if ((n & 1) == 1) {
              count++;
            }
            n = n >> 1; //right shift 1 bit
          }
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
          unsigned long n = e->_mask;
          for (int i = 0; n != 0; i++) {
            if ((n & 1) == 1) {
              addresses.push_back(calcAddr(e,i));
            }
            n = n >> 1; //right shift 1 bit
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
          int i = 0, _n = 0;
          for (; e->_mask != 0 && _n < n; i++) {
            if (e->_mask & (1<<i)) {
              addresses.push_back(calcAddr(e,i));
              _n++;
              e->_mask &= ~(1<<i);
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
            eof(false)
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
        const unsigned int entry_size;

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


      void record(const PrefetchInfo& pfi);

      void replay(const PrefetchInfo& pfi,
        std::vector<AddrPriority>& addresses);

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


      const int degree;
      /** The size of one spatial region. */
      const unsigned regionSize;
      /** log_2(block size spatil region). */
      unsigned lRegionSize;
      /** Number of blocks that fit in one region */
      unsigned blksPerRegion;

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


    public:




      void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override;

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

      void squashPrefetches(Addr addr, bool is_secure);

      void startRecord();
      bool startReplay();

    public:
      void initReplay(std::string filename);

    public:
      void dumpRecTrace(std::string filename);


















    protected:
      struct IStreamRecStats : public statistics::Group
      {
        IStreamRecStats(IStream* parent, const std::string& name);

        /** Number of hits and misses in the record buffer */
        statistics::Scalar hits;
        statistics::Scalar misses;

        // /** The hitting entries average distance to the head of
        //  *  the record buffer */
        // statistics::Formula avgHitDistance;

        // Record buffer hits distance distribution
        statistics::Histogram bufferHitDistHist;

        /** Number of writes and drops of entires to the record trace */
        statistics::Scalar entryWrites;
        statistics::Scalar entryDrops;
        statistics::Scalar entryOverflows;

        /** Average usefullness of a entry that falls out of the fifo buffer */
        statistics::Scalar cumUsefullness;
        statistics::Formula avgUsefullness;

        // Hit stack distance histograms
        statistics::Histogram hitSDlinHist;
        statistics::SparseHistogram hitSDlogHist;

      } recordStats;

      struct IStreamReplStats : public statistics::Group
      {
        IStreamReplStats(IStream* parent, const std::string& name);

        /** Number of hits and misses in the replay buffer */
        statistics::Scalar hits;
        statistics::Scalar misses;

        // /** The hitting entries average distance to the head of
        //  *  the record buffer */
        // statistics::Formula avgHitDistance;

        // Record buffer hits distance distribution
        statistics::Histogram bufferHitDistHist;

        /** Number of writes and drops of entires to the record trace */
        statistics::Scalar entryWrites;
        statistics::Scalar entryDrops;
        // Average usefullness of a entry that falls out of the fifo buffer
        statistics::Scalar cumUsefullness;
        statistics::Formula avgUsefullness;

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

      // unsigned cumHitDistance





    //   /**
    //      * Probe Listener to handle probe events from the CPU
    //      */
    //   class PrefetchListenerPC : public ProbeListenerArgBase<Addr>
    //   {
    //   public:
    //     PrefetchListenerPC(IStream& _parent, ProbeManager* pm,
    //       const std::string& name)
    //       : ProbeListenerArgBase(pm, name),
    //       parent(_parent)
    //     {
    //     }
    //     void notify(const Addr& pc) override;
    //   protected:
    //     IStream& parent;
    //   };

    //   /** Array of probe listeners */
    //   std::vector<PrefetchListenerPC*> listenersPC;



    //   /**
    //          * Add a SimObject and a probe name to monitor the retired
    //  instructions
    //          * @param obj The SimObject pointer to listen from
    //          * @param name The probe name
    //          */
    //   void addEventProbeRetiredInsts(SimObject* obj, const char* name);

    //   /**
    //        * Updates the prefetcher structures upon an instruction retired
    //        * @param pc PC of the instruction being retired
    //        */
    //   void notifyRetiredInst(const Addr pc);


    };

  } // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_ISTREAM_HH__
