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
#include "mem/cache/prefetch/queued.hh"
#include "mem/packet.hh"

namespace gem5
{

  struct IStreamPrefetcherParams;

  GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
  namespace prefetch
  {

    class IStream : public Queued
    {
      class Buffer;

      struct BufferEntry
      {
        private:
        // The parent buffer this entry belongs to.
        Buffer& parent;
        public:
        Addr _pageAddr;
        unsigned long _clMask;
        // TODO: This with parameter
        const unsigned MASK_SIZE = 64;
        const unsigned blkSize = 64;
        // const unsigned regionSize;

        BufferEntry(Buffer& _parent) : parent(_parent) {};
        BufferEntry(Buffer& _parent, Addr a, int mask = 0)
          : parent(_parent),_pageAddr(a), _clMask(mask)
        {
        }
        // BufferEntry& operator=(const BufferEntry& other) { return *this; }

        /** Marks a block in the region as touched */
        void touch(uint idx)
        {
          assert(idx < MASK_SIZE);
          _clMask |= (1 << idx);
        }

        bool check(uint idx)
        {
          return (_clMask & (1 << idx)) != 0;
        }

        bool empty() { return _clMask == 0; }
        // bool check(Addr addr)
        // {
        //   return addr;
        // }

        float usage()
        {
          int count = 0;
          unsigned long n = _clMask;
          while (n != 0) {
            if ((n & 1) == 1) {
              count++;
            }
            n = n >> 1; //right shift 1 bit
          }
          return (float)count / (float)parent.regionSize;
        }

        /** Calculate the address for a given index */
        Addr calcAddr(uint idx)
        {
          assert(idx < parent.regionSize);
          return _pageAddr + idx * parent.blkSize;
        }
        /**
         * This function will generate all block addresses that where recorded
         * to been accessed in this region.
        */
        int genAllAddr(std::vector<Addr>& addresses)
        {
          unsigned long n = _clMask;
          for (int i = 0; n != 0; i++) {
            if ((n & 1) == 1) {
              addresses.push_back(calcAddr(i));
            }
            n = n >> 1; //right shift 1 bit
          }
          return addresses.size();
        }

        /**
         * This function will generate up to n new address from this entry.
         * For each generated address it will clear the bit in the mask
         *
         * @param n The number of addresses to be generated.
         * @param addresses The address list where the new entry
         * should be append.
          * @return The number of addresses that where actually generated.
          */
        int genNAddr(int n, std::vector<Addr>& addresses)
        {
          int i = 0, _n = 0;
          for (; _clMask != 0 && _n < n; i++) {
            if (_clMask & (1<<i)) {
              addresses.push_back(calcAddr(i));
              _n++;
              _clMask &= ~(1<<i);
            }
          }
          return _n;
        }

        // /**
        //  * This function will generate up to n addresses
        //  * from this entry. It will mark the entries as
        // */
        // int genNAddr(int n, std::vector<Addr>& addresses)
        // {
        //   unsigned long n = _clMask;
        //   for (int i = 0; n != 0; i++) {
        //     if ((n & 1) == 1) {
        //       addresses.push_back(calcAddr(i));
        //     }
        //     n = n >> 1; //right shift 1 bit
        //   }
        //   return addresses.size();
        // }

        std::string print() {
          char buf[100];
          std::sprintf(buf, "RA: %#lx Mask: %lx", _pageAddr, _clMask);
          return std::string(buf);
        }
      };

      class Buffer
      {
        public:
        IStream& parent;
        const unsigned bufferEntries;
        const unsigned regionSize;
        const unsigned blkSize;


        // public:
          Buffer(IStream& _parent, unsigned size,
            unsigned _regionSize = 64, unsigned _blkSize = 64)
          :  parent(_parent),
            bufferEntries(size),
            regionSize(_regionSize),
            blkSize(_blkSize) {}
      };


      class RecordBuffer : public Buffer
      {
        std::deque<BufferEntry*> _buffer;
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
          _buffer.push_front(new BufferEntry(*this,a));
        }
        // TODO: Implement
        void flush();
        void flushAll();
      };

      class ReplayBuffer : public Buffer
      {
      private:
        std::list<BufferEntry*> _buffer;
        /** Number of entries in the FIFO buffer */

        /** Flag in case the end of the trace file was reached. */
        bool eof;

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
         * Fills the replay buffer by reading the instruction trace.
         * Returns the number of entires read from the file.
         */
        int fill();

        bool empty() { return _buffer.empty(); }
        bool traceEnd() { return eof; }

        std::string print() {
          std::string s = "[";
          for (auto it : _buffer) {
            s.append(it->print() + " | ");
          }
          return s + "]";
        }
      };

      class TraceStream
      {
      public:

        /**
         * Create an output stream for a given file name.
         * @param filename Path to the file to create or truncate
         */
        TraceStream(const std::string& filename, std::ios_base::openmode mode);
        ~TraceStream();

        /**
         * Read or write a buffer entry to the trace stream.
         *
         * @param entry The entry that should be written to the file.
         */
        void write(BufferEntry* entry);
        bool read(BufferEntry* entry);

        std::string name() {
          return ".tracestream";
        }

        void reset();

      private:
        /// Underlying file output stream
        std::fstream fileStream;
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
      TraceStream* recordStream;
      TraceStream* replayStream;
      RecordBuffer* buf;
      ReplayBuffer* replayBuffer;


      const int degree;
      /** The size of one spatial region. */
      const unsigned regionSize;

      /** log_2(block size spatil region). */
      unsigned lRegionSize;


      /**
       * Callback to flush and close all open output streams on exit. If
       * we were calling the destructor it could be done there.
       */
      void closeStreams();

    public:
      IStream(const IStreamPrefetcherParams& p);
      ~IStream() = default;

      void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override;

      void calculatePrefetch(const PrefetchInfo& pfi,
        std::vector<AddrPriority>& addresses);

      /**
       * Method to insert create a new prefetch request.
       */
      void insert(const PacketPtr &pkt,
        PrefetchInfo &new_pfi, int32_t priority);

      /**
       * Calculate the traget physical address (PA).
       */
      void proceedWithTranslation(const PacketPtr &pkt,
        PrefetchInfo &new_pfi, int32_t priority);

      void squashPrefetches(Addr addr, bool is_secure);

      void startRecord();

      void startReplay();






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
        // Average usefullness of a entry that falls out of the fifo buffer
        statistics::Scalar cumUsefullness;
        statistics::Formula avgUsefullness;


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
