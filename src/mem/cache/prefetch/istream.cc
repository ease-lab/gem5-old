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
  * Describes a IStream prefetcher based on template policies.
  */

#include "mem/cache/prefetch/istream.hh"

#include "debug/HWPrefetch.hh"
#include "params/IStreamPrefetcher.hh"

namespace gem5
{

  GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
  namespace prefetch
  {

    IStream::IStream(const IStreamPrefetcherParams& p)
      : Queued(p),
      traceStream(nullptr),
      degree(p.degree),
      regionSize(p.region_size),
      recordStats(this, ".recordStats"),
      replayStats(this, ".replayStats")
    {
      traceStream = new TraceStream(p.trace_file);
      buf = new RecordBuffer(*this, p.buffer_entries);
      replayBuffer = new ReplayBuffer(*this, p.buffer_entries);
      lRegionSize = floorLog2(regionSize);

      // Register a callback to compensate for the destructor not
      // being called. The callback forces the stream to flush and
      // closes the output file.
      registerExitCallback([this]() { closeStreams(); });
    }



    void
      IStream::closeStreams()
    {
      if (traceStream != NULL)
        delete traceStream;
    }

    void
      IStream::calculatePrefetch(const PrefetchInfo& pfi,
        std::vector<AddrPriority>& addresses)
    {
      record(pfi);

      Addr blkAddr = blockAddress(pfi.getAddr());

      DPRINTF(HWPrefetch, "Access to block %#x, pAddress %#x\n",
        blkAddr, pfi.getPaddr());

      for (int d = 1; d <= degree; d++) {
        Addr newAddr = blkAddr + d * (blkSize);
        addresses.push_back(AddrPriority(newAddr, 0));
      }

      replay(pfi, addresses);

    }

    Addr
      IStream::regionAddress(Addr a)
    {
      return a & ~((Addr)regionSize - 1);
    }

    unsigned
      IStream::regionIndex(Addr a)
    {
      return (a % regionSize) >> lBlkSize;
    }


    std::pair<Addr, unsigned>
      IStream::regionAddrIdx(Addr addr)
    {
      return std::make_pair(regionAddress(addr), regionIndex(addr));
    }

    void
      IStream::record(const PrefetchInfo& pfi)
    {
      Addr blkAddr = blockAddress(pfi.getAddr());
      auto region = regionAddrIdx(blkAddr);
      DPRINTF(HWPrefetch, "Record blk access [%#x] -> Region (%#x:%u)\n",
        blkAddr, region.first, region.second);

      buf->access(blkAddr);

    }

    void
      IStream::replay(const PrefetchInfo& pfi,
        std::vector<AddrPriority>& addresses)
    {
      std::vector<Addr> _tmpAddresses;
      Addr blkAddr = blockAddress(pfi.getAddr());
      // In case there is an entry in the reply buffer for this address
        // then we are already to late. We want to fetch the other addresses as
        // soon as possible.

      DPRINTF(HWPrefetch, "pfq size: %u\n", pfq.size());
      replayBuffer->access(blkAddr, _tmpAddresses);



    }


    void
      IStream::startRecord()
    {
      DPRINTF(HWPrefetch, "Start Recoding instructions\n");

      for (auto i : { 0,3,4,5 }) {
        buffer.push_back(BufferEntry((Addr)i, 0));
      }
      for (auto entry : buffer) {
        traceStream->write(&entry);
      }
      // traceStream->write("Hallo");
    }


    void
      IStream::startReplay()
    {
      DPRINTF(HWPrefetch, "Start Replay\n");

      buffer.clear();
      traceStream->reset();

      BufferEntry tmp;
      while (traceStream->read(&tmp)) {
        buffer.push_back(tmp);
        tmp.print();
      }
      for (auto e : buffer) {
        e.print();
      }
    }




    /**
         * Record Buffer
         */
    void
      IStream::RecordBuffer::access(gem5::Addr addr)
    {
      auto region = parent.regionAddrIdx(addr);

      // Lookup if the buffer contains already an entry for this region
      // auto it = std::find_if(_buffer.begin(), _buffer.end(),
      //   [region](BufferEntry e) -> bool {
      //     return region.first == e._pageAddr;
      //   });
      int hit_idx = 0;
      for (; hit_idx < _buffer.size(); hit_idx++) {
        if (_buffer[hit_idx]->_pageAddr == region.first) {
          break;
        }
      }


      if (hit_idx < _buffer.size()) {
        parent.recordStats.hits++;
        parent.recordStats.bufferHitDistHist.sample(hit_idx);
        DPRINTF(HWPrefetch, "[%#x] Rec. buffer hit: Region (%#x:%u)\n",
          addr, _buffer[hit_idx]->_pageAddr, region.second);
      }
      else {
        parent.recordStats.misses++;
        DPRINTF(HWPrefetch, "[%#x] Rec. buffer miss: Region (%#x:%u)\n",
          addr, _buffer[hit_idx]->_pageAddr, region.second);

        // Create new entry and push it into the fifo buffer
        _buffer.push_front(new BufferEntry(region.first));
        hit_idx = 0;
      }
      // Mark the entry block in the region as used
      _buffer[hit_idx]->touch(region.second);

      // If the buffer is full write the last entry to the file.
      if (_buffer.size() > bufferEntries) {
        auto wrEntry = _buffer.back();
        auto usage = wrEntry->usage();
        parent.recordStats.cumUsefullness += usage;

        // TODO parameter
        const float threshold = 0;
        if (usage > threshold) {
          parent.recordStats.entryWrites++;
          DPRINTF(HWPrefetch, "Rec. buffer entry %#x:[%x]: write to file."
            " Usage: %.3f\n", wrEntry->_pageAddr, wrEntry->_clMask, usage);
          parent.traceStream->write(wrEntry);
        }
        else {
          parent.recordStats.entryDrops++;
          DPRINTF(HWPrefetch, "Drop buffer entry [%#x:%x] drop. Usage: %.3f\n",
            wrEntry->_pageAddr, wrEntry->_clMask, usage);
        }
        _buffer.pop_back();
      }
    }


    /**
     * Replay Buffer functionality
     */


     /**
      * Check if there is an entry for this address in the replay buffer.
      * If so the all addresses will be generated for prefetching.
      * Finally, the entry can be dropped.
      */

    void
      IStream::ReplayBuffer::access(gem5::Addr addr,
        std::vector<Addr>& addresses)
    {
      auto region = parent.regionAddrIdx(addr);

      // Check if the replay buffer contains the entry for this region
      int hit_idx = 0;
      for (; hit_idx < _buffer.size(); hit_idx++) {
        if (_buffer[hit_idx]->_pageAddr == region.first
          && _buffer[hit_idx]->check(region.second)) {
          break;
        }
      }

      if (hit_idx == _buffer.size()) {
        parent.replayStats.misses++;
        DPRINTF(HWPrefetch, "[%#x] Repl. buffer miss: Region (%#x:%u)\n",
          addr, _buffer[hit_idx]->_pageAddr, region.second);
        return;
      }

      parent.replayStats.hits++;
      parent.replayStats.bufferHitDistHist.sample(hit_idx);
      DPRINTF(HWPrefetch, "[%#x] Repl. buffer hit: Region (%#x:%u)\n",
        addr, _buffer[hit_idx]->_pageAddr, region.second);


      // Generate all addresses from this entry
      _buffer[hit_idx]->genAllAddr(addresses);

      // And drop the entry
      // auto it = ;
      _buffer.erase(_buffer.begin());
    }


    void
      IStream::ReplayBuffer::generateAddresses(BufferEntry* entry)
    {
      // std::vector<Addr> addresses;

    }





    IStream::TraceStream::TraceStream(const std::string& filename) :
      fileStream(filename.c_str(),
        std::ios::out | std::ios::in | std::ios::binary | std::ios::trunc)
    {
      if (!fileStream.good())
        panic("Could not open %s for writing\n", filename);
    }

    IStream::TraceStream::~TraceStream()
    {
      // As the compression is optional, see if the stream exists
      // if (gzipStream != NULL)
      //     delete gzipStream;
      // delete wrappedFileStream;
      fileStream.close();
    }

    void
      IStream::TraceStream::write(BufferEntry* entry)
    {
      fileStream.write((char*)entry, sizeof(*entry));
    }

    bool
      IStream::TraceStream::read(BufferEntry* entry)
    {
      if (!fileStream.eof()) {
        fileStream.read((char*)entry, sizeof(*entry));
        return true;
      }
      return false;
    }
    void
      IStream::TraceStream::reset()
    {
      // seek to the start of the input file and clear any flags
      fileStream.clear();
      fileStream.seekg(0, std::ifstream::beg);
    }






    IStream::IStreamRecStats::IStreamRecStats(IStream* parent,
      const std::string& name)
      : statistics::Group(parent, name.c_str()),
      ADD_STAT(hits, statistics::units::Count::get(),
        "Number of hits in the record buffer"),
      ADD_STAT(misses, statistics::units::Count::get(),
        "Number of misses in the record buffer"),
      ADD_STAT(bufferHitDistHist, statistics::units::Count::get(),
        "Record buffer hits distance distribution"),
      ADD_STAT(entryWrites, statistics::units::Count::get(),
        "Number of entries written to the record trace"),
      ADD_STAT(entryDrops, statistics::units::Count::get(),
        "Number of entires dropped to write to the record trace"),
      ADD_STAT(cumUsefullness, statistics::units::Count::get(),
        "Cummulative usefullness of the entries that fall out of the "
        "fifo buffer"),
      ADD_STAT(avgUsefullness, statistics::units::Count::get(),
        "Average usefullness of the entries that fall out of the fifo buffer")
    {
      using namespace statistics;

      const IStreamPrefetcherParams& p =
        dynamic_cast<const IStreamPrefetcherParams&>(parent->params());

      bufferHitDistHist
        .init(p.buffer_entries)
        .flags(pdf);

      avgUsefullness.flags(total);
      avgUsefullness = cumUsefullness / (entryWrites + entryDrops);

    }

    IStream::IStreamReplStats::IStreamReplStats(IStream* parent,
      const std::string& name)
      : statistics::Group(parent, name.c_str()),
      ADD_STAT(hits, statistics::units::Count::get(),
        "Number of hits in the record buffer"),
      ADD_STAT(misses, statistics::units::Count::get(),
        "Number of misses in the record buffer"),
      // ADD_STAT(avgHitDistance, statistics::units::Count::get(),
      //   "The hitting entries average distance to the head of "
            // "the record buffer"),
      ADD_STAT(bufferHitDistHist, statistics::units::Count::get(),
        "Record buffer hits distance distribution"),
      ADD_STAT(entryWrites, statistics::units::Count::get(),
        "Number of entries written to the record trace"),
      ADD_STAT(entryDrops, statistics::units::Count::get(),
        "Number of entires dropped to write to the record trace"),
      ADD_STAT(cumUsefullness, statistics::units::Count::get(),
        "Cummulative usefullness of the entries that fall out of the "
        "fifo buffer"),
      ADD_STAT(avgUsefullness, statistics::units::Count::get(),
        "Average usefullness of the entries that fall out of the fifo buffer")
    {
      using namespace statistics;

      const IStreamPrefetcherParams& p =
        dynamic_cast<const IStreamPrefetcherParams&>(parent->params());

      bufferHitDistHist
        .init(p.buffer_entries)
        .flags(pdf);

      avgUsefullness.flags(total);
      avgUsefullness = cumUsefullness / (entryWrites + entryDrops);

    }

























    // void
    //   IStream::notifyRetiredInst(const Addr pc)
    // {
    //   DPRINTF(HWPrefetch, "Notify pf about retired inst: %#x ",
    //     pc);
    // }


    // void
    //   IStream::PrefetchListenerPC::notify(const Addr& pc)
    // {
    //   parent.notifyRetiredInst(pc);
    // }

    // void
    //   IStream::addEventProbeRetiredInsts(SimObject* obj, const char* name)
    // {
    //   ProbeManager* pm(obj->getProbeManager());
    //   // DPRINTF(HWPrefetch, "Add Retired Instruction listener: % %s ",
    //   //                     &obj, name);
    //   listenersPC.push_back(new PrefetchListenerPC(*this, pm, name));
    // }


  } // namespace prefetch
} // namespace gem5
