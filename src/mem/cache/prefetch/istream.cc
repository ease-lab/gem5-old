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
#include "mem/cache/base.hh"
#include "params/IStreamPrefetcher.hh"

namespace gem5
{

  GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
  namespace prefetch
  {

    IStream::IStream(const IStreamPrefetcherParams& p)
      : Queued(p),
      recordStream(nullptr),
      replayStream(nullptr),
      degree(p.degree),
      regionSize(p.region_size),
      recordStats(this, ".recordStats"),
      replayStats(this, ".replayStats")
    {
      recordStream = new TraceStream(p.trace_record_file, std::ios::out);
      replayStream = new TraceStream(p.trace_replay_file, std::ios::in);
      buf = new RecordBuffer(*this, p.buffer_entries, regionSize);
      replayBuffer = new ReplayBuffer(*this, p.buffer_entries, regionSize);
      lRegionSize = floorLog2(regionSize);

      // Register a callback to compensate for the destructor not
      // being called. The callback forces the stream to flush and
      // closes the output file.
      registerExitCallback([this]() { closeStreams(); });
    }



    void
      IStream::closeStreams()
    {
      if (recordStream != NULL)
        delete recordStream;
      if (replayStream != NULL)
        delete replayStream;
    }

    void
      IStream::calculatePrefetch(const PrefetchInfo& pfi,
        std::vector<AddrPriority>& addresses)
    {
      record(pfi);

      Addr blkAddr = blockAddress(pfi.getAddr());

      DPRINTF(HWPrefetch, "Access to block %#x, pAddress %#x\n",
        blkAddr, pfi.getPaddr());

      // for (int d = 1; d <= degree; d++) {
      //   Addr newAddr = blkAddr + d * (blkSize);
      //   addresses.push_back(AddrPriority(newAddr, 0));
      // }

      replay(pfi, addresses);

    }

    void IStream::squashPrefetches(Addr addr, bool is_secure)
    {
      // Squash queued prefetches if demand miss to same line
      if (queueSquash) {
        auto itr = pfq.begin();
        while (itr != pfq.end()) {
          if (itr->pfInfo.getAddr() == addr &&
            itr->pfInfo.isSecure() == is_secure) {
            DPRINTF(HWPrefetch, "Removing pf candidate addr: %#x "
              "(cl: %#x), demand request going to the same addr\n",
              itr->pfInfo.getAddr(),
              blockAddress(itr->pfInfo.getAddr()));
            delete itr->pkt;
            itr = pfq.erase(itr);
            statsQueued.pfRemovedDemand++;
          }
          else {
            ++itr;
          }
        }
      }
    }

    void
      IStream::notify(const PacketPtr& pkt, const PrefetchInfo& pfi)
    {
      if (useVirtualAddresses && (tlb == nullptr)) {
        DPRINTF(HWPrefetch, "Using virtual for Record and replay "
          " requires to register a TLB.\n");
        return;
      }


      Addr blk_addr = blockAddress(pfi.getAddr());
      // Incase we see a request to the same address the prefetch
      // would be to late.
      squashPrefetches(blk_addr, pfi.isSecure());

      // Calculate prefetches given this access
      std::vector<AddrPriority> addresses;
      calculatePrefetch(pfi, addresses);

      // Get the maximum number of prefetches that we are allowed to generate
      size_t max_pfs = getMaxPermittedPrefetches(addresses.size());

      // Queue up generated prefetches
      size_t num_pfs = 0;
      for (AddrPriority& addr_prio : addresses) {

        // Block align prefetch address
        addr_prio.first = blockAddress(addr_prio.first);

        if (!samePage(addr_prio.first, pfi.getAddr())) {
          statsQueued.pfSpanPage += 1;
        }

        PrefetchInfo new_pfi(pfi, addr_prio.first);
        statsQueued.pfIdentified++;
        DPRINTF(HWPrefetch, "Found a pf candidate addr: %#x, "
          "inserting into prefetch queue.\n", new_pfi.getAddr());
        // Create and insert the request
        insert(pkt, new_pfi, addr_prio.second);
        num_pfs += 1;
        if (num_pfs == max_pfs) {
          break;
        }

      }
      // Now translation can be started if necessary.
      processMissingTranslations(queueSize - pfq.size());
    }


    void
      IStream::proceedWithTranslation(const PacketPtr& pkt,
        PrefetchInfo& new_pfi, int32_t priority)
    {
      /*
       * Physical address computation
       * As we record addresses we only need to compute the PA
       * in case we recorded virtual once.
       */

       // ContextID is needed for translation
      if (!pkt->req->hasContextId()) {
        return;
      }

      RequestPtr translation_req = createPrefetchRequest(
        new_pfi.getAddr(), new_pfi, pkt);

      /* Create the packet and find the spot to insert it */
      DeferredPacket dpp(this, new_pfi, 0, priority);

      // Add the translation request.
      dpp.setTranslationRequest(translation_req);
      dpp.tc = cache->system->threads[translation_req->contextId()];
      DPRINTF(HWPrefetch, "Prefetch queued with no translation. "
        "addr:%#x priority: %3d\n", new_pfi.getAddr(), priority);
      addToQueue(pfqMissingTranslation, dpp);
    }



    void
      IStream::insert(const PacketPtr& pkt,
        PrefetchInfo& new_pfi, int32_t priority)
    {
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

      if (cacheSnoop &&
        (inCache(target_paddr, new_pfi.isSecure()) ||
          inMissQueue(target_paddr, new_pfi.isSecure()))) {
        statsQueued.pfInCache++;
        DPRINTF(HWPrefetch, "Dropping redundant in "
          "cache/MSHR prefetch addr:%#x\n", target_paddr);
        return;
      }

      /* Create the packet and insert it in the prefetch queue */
      DeferredPacket dpp(this, new_pfi, 0, priority);

      Tick pf_time = curTick() + clockPeriod() * latency;
      dpp.createPkt(target_paddr, blkSize, requestorId,
        tagPrefetch, pf_time);
      DPRINTF(HWPrefetch, "Prefetch queued. "
        "addr:%#x priority: %3d tick:%lld.\n",
        new_pfi.getAddr(), priority, pf_time);
      addToQueue(pfq, dpp);
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
      buf->flush();

    }

    void
      IStream::replay(const PrefetchInfo& pfi,
        std::vector<AddrPriority>& addresses)
    {
      // TODO: Fill up timely
      replayBuffer->fill();
      if (replayBuffer->empty()) {
        DPRINTF(HWPrefetch, "Reached end of trace file. No more records "
          "for prefetching \n");
        return;
      }

      std::vector<Addr> _tmpAddresses;
      Addr blkAddr = blockAddress(pfi.getAddr());
      // In case there is an entry in the reply buffer for this address
        // then we are already to late. We want to fetch the other addresses as
        // soon as possible.

      DPRINTF(HWPrefetch, "pfq size: %u, pfqMissingTr size: %u\n", pfq.size(),
        pfqMissingTranslation.size());


      // Test if there is an entry in the replay buffer that meats this
      // address.
      replayBuffer->probe(blkAddr);
      DPRINTF(HWPrefetch, "Repl. Buff: %s\n", replayBuffer->print());
      // Generate new addresses
      int max = queueSize - pfq.size() - pfqMissingTranslation.size() - 16;
      if (max > 0) {
        auto _addresses = replayBuffer->generateNAddresses(4);
        // Pass them to the prefetch queue.
        // TODO: add priority
        for (auto addr : _addresses) {
          addresses.push_back(AddrPriority(addr, 0));
        }
      }

      DPRINTF(HWPrefetch, "Repl. Buff: %s\n", replayBuffer->print());
    }


    void
      IStream::startRecord()
    {
      // DPRINTF(HWPrefetch, "Start Recoding instructions\n");

      // for (auto i : { 0,3,4,5 }) {
      //   buffer.push_back(BufferEntry((Addr)i, 0));
      // }
      // for (auto entry : buffer) {
      //   recordStream->write(&entry);
      // }
      // // traceStream->write("Hallo");
    }


    void
      IStream::startReplay()
    {
      // DPRINTF(HWPrefetch, "Start Replay\n");

      // buffer.clear();
      // traceStream->reset();

      // BufferEntry tmp;
      // while (traceStream->read(&tmp)) {
      //   buffer.push_back(tmp);
      //   tmp.print();
      // }
      // for (auto e : buffer) {
      //   e.print();
      // }
    }


    /*****************************************************************
     * Record Buffer functionality
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
        DPRINTF(HWPrefetch, "[%#x] hit: Region (%#x:%u)\n",
          addr, _buffer[hit_idx]->_pageAddr, region.second);
      }
      else {
        parent.recordStats.misses++;
        DPRINTF(HWPrefetch, "[%#x] miss. "
          "Add new entry to the record buffer\n", addr);

        // Create new entry and push it into the fifo buffer
        add(region.first);
        hit_idx = 0;
      }
      // Mark the entry block in the region as used
      _buffer[hit_idx]->touch(region.second);
    }

    void
      IStream::RecordBuffer::flush()
    {
      // Resize the buffer by writting the last
      // entries into the trace file.
      while (_buffer.size() > bufferEntries) {
        auto wrEntry = _buffer.back();
        auto usage = wrEntry->usage();
        parent.recordStats.cumUsefullness += usage;

        // TODO parameter
        const float threshold = 0;
        if (usage > threshold) {
          parent.recordStats.entryWrites++;
          DPRINTF(HWPrefetch, "Rec. buffer entry %#x:[%x]: write to file."
            " Usage: %.3f\n", wrEntry->_pageAddr, wrEntry->_clMask, usage);
          parent.recordStream->write(wrEntry);
        }
        else {
          parent.recordStats.entryDrops++;
          DPRINTF(HWPrefetch, "Drop buffer entry [%#x:%x] drop. Usage: %.3f\n",
            wrEntry->_pageAddr, wrEntry->_clMask, usage);
        }
        _buffer.pop_back();
      }
    }


    /*****************************************************************
     * Replay Buffer functionality
     */


     /**
      * Check if there is an entry for this address in the replay buffer.
      * If so the all addresses will be generated for prefetching.
      * Finally, the entry can be dropped.
      */

    void
      IStream::ReplayBuffer::probe(gem5::Addr addr)
    {
      auto region = parent.regionAddrIdx(addr);

      // Check if the replay buffer contains the entry for this region
      int hit_idx = 0;
      auto it = _buffer.begin();
      for (; it != _buffer.end(); it++, hit_idx++) {
        if ((*it)->_pageAddr == region.first
          && (*it)->check(region.second)) {
          break;
        }
      }

      if (it == _buffer.end()) {
        return;
      }

      parent.replayStats.hits++;
      parent.replayStats.bufferHitDistHist.sample(hit_idx);
      DPRINTF(HWPrefetch, "hit: Region (%#x:%u). Move to front.\n",
        (*it)->_pageAddr, region.second);

      // In the case we found an entry within the replay buffer we are
      // already to late to for this to be prefetched.
      // However, then we will try to prefetch the other once as soon as
      // possible. Therefore, we put this entry to the head of the replay
      // buffer.
      _buffer.splice(_buffer.begin(), _buffer, it);
      // _buffer[hit_idx]->genAllAddr(addresses);

      // // And drop the entry
      // _buffer.erase(_buffer.begin() + hit_idx);
    }


    std::vector<Addr>
      IStream::ReplayBuffer::generateNAddresses(int n)
    {
      std::vector<Addr> addresses;
      int n_left = n;

      // Starting from the beginning of the list generate
      // up to n addresses.
      for (auto it = _buffer.begin(); it != _buffer.end();) {
        n_left -= (*it)->genNAddr(n_left, addresses);
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

    int
      IStream::ReplayBuffer::fill()
    {
      int n = 0;
      while (_buffer.size() < bufferEntries) {

        auto tmp = new BufferEntry(*this);
        if (!parent.replayStream->read(tmp)) {
          DPRINTF(HWPrefetch, "Reached end of file.\n");
          eof = true;
          return n;
        }

        n++;
        DPRINTF(HWPrefetch, "Fill: %s\n", tmp->print());
        _buffer.push_back(tmp);
      }
      return n;
    }





    IStream::TraceStream::TraceStream(const std::string& filename,
      std::ios_base::openmode mode) :
      fileStream(filename.c_str(), mode)
    {
      if (!fileStream.good())
        panic("Could not open %s\n", filename);
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
      // DPRINTF(HWPrefetch, "WR: %s\n",entry->print());
      // std::getline(sstream, record);
      fileStream << entry->_pageAddr << "," << entry->_clMask << "\n";



      // fileStream.write((char*)entry, sizeof(*entry));
    }

    bool
      IStream::TraceStream::read(BufferEntry* entry)
    {
      if (!fileStream.eof()) {
        // fileStream.read((char*)entry, sizeof(*entry));

        std::string record;
        std::getline(fileStream, record);
        std::istringstream line(record);
        if (fileStream.eof()) {
          return false;
        }

        std::getline(line, record, ',');
        entry->_pageAddr = Addr(std::stoull(record));
        std::getline(line, record, ',');
        entry->_clMask = std::stoull(record);

        // DPRINTF(HWPrefetch, "RD: %s\n",entry->print());
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
