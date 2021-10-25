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

      struct BufferEntry
      {
        Addr _pageAddr;
        unsigned long _clMask;
        // TODO: This with parameter
        const unsigned MASK_SIZE = 64;
        const unsigned blkSize = 64;
        const unsigned regionSize = 4;

        BufferEntry() {};
        BufferEntry(Addr a, int mask = 0)
          : _pageAddr(a), _clMask(mask)
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
          return (float)count / (float)MASK_SIZE;
        }

        /** Calculate the address for a given index */
        Addr calcAddr(uint idx)
        {
          assert(idx < MASK_SIZE);
          return _pageAddr + idx * blkSize;
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

        void print()
        {
          std::cout << "PA: " << _pageAddr << " ," << _clMask << std::endl;
        }
      };


      class RecordBuffer
      {
        std::deque<BufferEntry*> _buffer;
        /** Number of entries in the FIFO buffer */
        const unsigned bufferEntries;
        IStream& parent;

        std::string name()
        {
          return parent.name() + ".recBuffer";
        }

      public:
        RecordBuffer(IStream& _parent, unsigned size)
          : bufferEntries(size),
          parent(_parent)
        {
        }

        void access(Addr addr);
      };

      class ReplayBuffer
      {
      private:
        std::deque<BufferEntry*> _buffer;
        /** Number of entries in the FIFO buffer */
        const unsigned bufferEntries;
        IStream& parent;

        std::string name()
        {
          return parent.name() + ".recBuffer";
        }

      public:
        ReplayBuffer(IStream& _parent, unsigned size)
          : bufferEntries(size),
          parent(_parent)
        {
        }

        void access(gem5::Addr addr, std::vector<Addr>& addresses);
        void generateAddresses(BufferEntry* entry);
      };

      class TraceStream
      {
      public:

        /**
         * Create an output stream for a given file name.
         * @param filename Path to the file to create or truncate
         */
        TraceStream(const std::string& filename);
        ~TraceStream();

        /**
         * Read or write a buffer entry to the trace stream.
         *
         * @param entry The entry that should be written to the file.
         */
        void write(BufferEntry* entry);
        bool read(BufferEntry* entry);

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
      TraceStream* traceStream;
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

      void calculatePrefetch(const PrefetchInfo& pfi,
        std::vector<AddrPriority>& addresses) override;






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
