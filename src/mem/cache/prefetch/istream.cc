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

#include "mem/cache/prefetch/istream.hh"

#include <algorithm>

#include "debug/HWPrefetch.hh"
#include "mem/cache/base.hh"
#include "params/IStreamPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

void
IStream::PrefetchListenerCS::notify(const bool &active)
{
  if (active && parent.waitForCSActive) {
    DPRINTF(HWPrefetch, "Notify Context switch from idle to active.\n");
      parent.waitForCSActive = false;
      parent.startRecord();
      parent.startReplay();
      if (parent.stopOnCS) {
        parent.waitForCSIdle = true;
      }
  }

  if (!active && parent.waitForCSIdle) {
    DPRINTF(HWPrefetch, "Notify Context switch from active to idle.\n");
    parent.stopRecord();
  }



}

IStream::IStream(const IStreamPrefetcherParams& p)
  : Queued(p),

  // recordStream(nullptr),
  // replayStream(nullptr),
  memInterface(nullptr),
  port(this, p.sys),
  // backdoor(name() + "-backdoor", *this),
  // dmaPort(this, p.sys),
  enable_record(false), enable_replay(false),
  startOnCS(p.start_on_context_switch),stopOnCS(p.stop_on_context_switch),
  waitForCSActive(false),waitForCSIdle(false),
  recordMissesOnly(p.record_misses_only),
  skipInCache(p.skip_in_cache),
  degree(p.degree),
  regionSize(p.region_size),
  pfRecorded(0), pfReplayed(0), totalPrefetches(0),
  pfIssued(0), pfUsed(0),pfUnused(0),
  replayDistance(p.replay_distance),

  recordStats(this, "recordStats"),
  replayStats(this, "replayStats"),
  memIFStats(this, "memIFStats")
{
  lRegionSize = floorLog2(regionSize);
  blksPerRegion = regionSize / blkSize;

  // Calculate the real entry size not the one we have for modeling.
  int bits_region_ptr = 48 - lRegionSize;
  entrySize_n_addr = ceil(float(bits_region_ptr) / 8);
  entrySize_n_mask = ceil(float(blksPerRegion) / 8);
  realEntrySize = entrySize_n_addr + entrySize_n_mask;



  DPRINTF(HWPrefetch, "Cfg: region size: %i, blk size %i => "
                "%i blks/region => Entry size: %ib/%iB (%iB + %iB)\n",
                    regionSize, blkSize, blksPerRegion,
                    bits_region_ptr + blksPerRegion,
                    realEntrySize, entrySize_n_addr, entrySize_n_mask);


  // recordStream = new TraceStream(p.trace_record_file);
  // replayStream = new TraceStream(p.trace_replay_file);
  memInterface = new MemoryInterface(*this,
                    p.record_addr_range,
                    p.replay_addr_range,
                    p.sys->physProxy);
  recordBuffer = new RecordBuffer(*this, p.record_buffer_entries, regionSize);
  replayBuffer = new ReplayBuffer(*this, p.replay_buffer_entries, regionSize);


  // Register a callback to compensate for the destructor not
  // being called. The callback forces the stream to flush and
  // closes the output file.
  // registerExitCallback([this]() { closeStreams(); });
}

void
IStream::init()
{
  if (!port.isConnected())
      fatal("IStream prefetcher need to be connected to memory.\n");
}

DrainState
IStream::drain()
{
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

    // void
    //   IStream::closeStreams()
    // {
    //   // if (recordStream != NULL)
    //   //   delete recordStream;
    //   // if (replayStream != NULL)
    //   //   delete replayStream;
    // }

    void
      IStream::calculatePrefetch(const PrefetchInfo& pfi,
        std::vector<AddrPriority>& addresses)
    {
      if (enable_record) {
        record(pfi);
      }


      Addr blkAddr = blockAddress(pfi.getAddr());

      // DPRINTF(HWPrefetch, "Access to block %#x, pAddress %#x\n",
      //   blkAddr, pfi.getPaddr());

      // for (int d = 1; d <= degree; d++) {
      //   Addr newAddr = blkAddr + d * (blkSize);
      //   addresses.push_back(AddrPriority(newAddr, 0));
      // }

      if (enable_replay) {
        replay(pfi, addresses);
      }
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
      if (hasBeenPrefetched(pkt->getAddr(), pkt->isSecure())) {
        pfUsed++;
        replayStats.pfUseful++;
        if (!inCache(pkt->getAddr(), pkt->isSecure()))
            // This case happens when a demand hits on a prefetched line
            // that's not in the requested coherency state.
            replayStats.pfUsefulButMiss++;
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
    replayStats.pfHitInCache++;
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

  replayStats.pfInsertedInQueue++;
  addToQueue(pfq, dpp);
}

void
IStream::translationComplete(DeferredPacket *dp, bool failed)
{
    auto it = pfqMissingTranslation.begin();
    while (it != pfqMissingTranslation.end()) {
        if (&(*it) == dp) {
            break;
        }
        it++;
    }
    assert(it != pfqMissingTranslation.end());
    if (!failed) {
        DPRINTF(HWPrefetch, "%s Translation of vaddr %#x succeeded: "
                "paddr %#x \n", tlb->name(),
                it->translationRequest->getVaddr(),
                it->translationRequest->getPaddr());
        Addr target_paddr = it->translationRequest->getPaddr();
        // check if this prefetch is already redundant
        if (cacheSnoop && (inCache(target_paddr, it->pfInfo.isSecure()) ||
                    inMissQueue(target_paddr, it->pfInfo.isSecure()))) {
            statsQueued.pfInCache++;
            replayStats.pfHitInCache++;
            DPRINTF(HWPrefetch, "Dropping redundant in "
                    "cache/MSHR prefetch addr:%#x\n", target_paddr);
        } else {
            Tick pf_time = curTick() + clockPeriod() * latency;
            it->createPkt(it->translationRequest->getPaddr(), blkSize,
                    requestorId, tagPrefetch, pf_time);
            replayStats.pfInsertedInQueue++;
            addToQueue(pfq, *it);
        }



    } else {
        DPRINTF(HWPrefetch, "%s Translation of vaddr %#x failed, dropping "
                "prefetch request %#x \n", tlb->name(),
                it->translationRequest->getVaddr());
        // statsQueued.pfTranslationFail++;
        replayStats.pfTranslationFail++;
        // replayStats.translationRetries++;
        // We will leave the deferred packet in the outstanding queue
        // and try to do the translation again.
    }
    pfqMissingTranslation.erase(it);
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
      recordStats.notifies++;
      Addr blkAddr = blockAddress(pfi.getAddr());
      auto region = regionAddrIdx(blkAddr);
      DPRINTF(HWPrefetch, "Record blk access [%#x] -> Region (%#x:%u)\n",
        blkAddr, region.first, region.second);

      // We will skip recording the address when the entry still
      // remain in the cache.
      //
      bool skip(false);
      // Only record cache misses
      if (recordMissesOnly && !pfi.isCacheMiss()) {
        // DPRINTF(HWPrefetch, "Skip recording address because "
        //     "cache hit:%#x\n", blkAddr);
        recordStats.cacheHit++;
        skip = true;
      }
      Addr pBlkAddr = blockAddress(pfi.getPaddr());
      if (cacheSnoop && (inCache(pBlkAddr, pfi.isSecure()))) {
        // DPRINTF(HWPrefetch, "Skip recording address because "
        //     "still in cache: %#x PA: %#x\n", blkAddr, pBlkAddr);
        recordStats.inCache++;
        if (skipInCache) {
          skip = true;
        }
      }
      if (cacheSnoop && (inMissQueue(pBlkAddr, pfi.isSecure()))) {
        // DPRINTF(HWPrefetch, "Address is in miss queue. Record it "
        //     "%#x PA: %#x\n", blkAddr, pBlkAddr);
        recordStats.hitInMissQueue++;
      }
      if (cacheSnoop && (hasBeenPrefetched(pBlkAddr, pfi.isSecure()))) {
      // DPRINTF(HWPrefetch, "Do not skip recording when this was a prefetch. "
      //     "Record it %#x PA: %#x\n", blkAddr, pBlkAddr);
        recordStats.cacheHitBecausePrefetch++;
        skip = false;
      }
      if (skip) {
        recordStats.accessDrops++;
        return;
      }

      recordBuffer->access(blkAddr);
      recordBuffer->flush();

    }

    void
      IStream::replay(const PrefetchInfo& pfi,
        std::vector<AddrPriority>& addresses)
    {

      // Pace down replay in case we are to far ahead.
      // This will avoid thrashing the cache.
      if (replayDistance >= 0) {
        DPRINTF(HWPrefetch, "Steer replay distance: "
                "Recorded/Replayed/Issued/Used/Unused prefetches: "
                "%u/%u/%u/%u/%u\n",
                  pfReplayed, pfRecorded, pfIssued, pfUsed, pfUnused);
        if (pfIssued >= pfUsed + pfUnused + replayDistance) {
          DPRINTF(HWPrefetch, "Slow down replaying.\n");
          return;
        }
      }



      if (!replayBuffer->refill()) {
        DPRINTF(HWPrefetch, "Reached end of trace file. No more records "
          "for prefetching \n");
        if (pfReplayed >= totalPrefetches) {
          DPRINTF(HWPrefetch, "All prefetches are issued. Replaying done!!\n");
          enable_replay = false;
          return;
        }
        DPRINTF(HWPrefetch, "Replay buffer still contains entries\n");
      }

      std::vector<Addr> _tmpAddresses;
      Addr blkAddr = blockAddress(pfi.getAddr());
      // In case there is an entry in the reply buffer for this address
        // then we are already to late. We want to fetch the other addresses as
        // soon as possible.

      DPRINTF(HWPrefetch, "pfq size: %u, pfqMissingTr size: %u\n",
                pfq.size(), pfqMissingTranslation.size());


      // Test if there is an entry in the replay buffer that meats this
      // address.
      replayBuffer->probe(blkAddr);
      DPRINTF(HWPrefetch, "Repl. Buff: %s\n", replayBuffer->print());
      // Generate new prefetch addresses.
      int max = queueSize - pfq.size() - pfqMissingTranslation.size();
      if (max > 0) {
        auto _addresses = replayBuffer->generateNAddresses(max);
        pfReplayed += _addresses.size();
        replayStats.pfGenerated += _addresses.size();
        DPRINTF(HWPrefetch, "Generated %i new pref. cand. (max: %i) (%i/%i)\n",
                      _addresses.size(), max, pfReplayed, totalPrefetches);
        // Pass them to the prefetch queue.
        // TODO: add priority
        for (auto addr : _addresses) {
          addresses.push_back(AddrPriority(addr, 0));
        }
      }

      DPRINTF(HWPrefetch, "Repl. Buff: %s\n", replayBuffer->print());
    }


void
  IStream::startRecord(bool wait_for_cs)
{
  if (wait_for_cs) {
    waitForCSActive = true;
    return;
  }
  DPRINTF(HWPrefetch, "Start Recoding instructions\n");
  // recordStream->reset();
  recordBuffer->clear();
  memInterface->resetRecord();
  pfRecorded = 0;
  enable_record = true;
}

bool
  IStream::startReplay(bool wait_for_cs)
{
  if (!memInterface->replayReady()) {
    warn("Cannot start replay because memory is not "
         "completely initialized.\n");
    return false;
  }
  if (wait_for_cs) {
    waitForCSActive = true;
    return true;
  }

  DPRINTF(HWPrefetch, "Start Replaying instructions\n");
  // recordStream->reset();
  memInterface->resetReplay();
  replayBuffer->clear();
  replayBuffer->refill();
  pfReplayed=0; pfIssued=0; pfUsed=0; pfUnused=0;
  enable_replay = true;
  return true;
}

void
  IStream::startAtScheduling()
{
  waitForCSActive = true;
  waitForCSIdle = false;
}




std::string
IStream::readFileIntoString(const std::string& filename) {
    auto ss = std::ostringstream{};
    std::ifstream input_file(filename);
    if (!input_file.is_open()) {
        std::cerr << "Could not open the file - '"
             << filename << "'" << std::endl;
        exit(EXIT_FAILURE);
    }
    ss << input_file.rdbuf();
    return ss.str();
}


void
  IStream::initReplay(std::string filename)
{
  std::string file_contents = readFileIntoString(filename);
  std::vector<BufferEntryPtr> trace;

  std::istringstream sstream(file_contents);
  std::string line;
  totalPrefetches = 0;
  while (std::getline(sstream,line)) {
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
  replayStats.totalPrefetchesInTrace += totalPrefetches;

  DPRINTF(HWPrefetch, "Read %i entries from %s: Init mem. Trace contains "
                  "%i prefetches\n", trace.size(), filename, totalPrefetches);
  // Write the entries to memory
  if (!memInterface->initReplayMem(trace)) {
    warn("Could not initialize replay memory correctly.\n");
  }
}




void
  IStream::dumpRecTrace(std::string filename)
{
  // Make sure the content is fully written to memory.
  recordBuffer->flush(true);

  // Get the entries from the memory interface.
  std::vector<BufferEntryPtr> entries;
  memInterface->readRecordMem(entries);

  // Write the entries into the give file
  std::fstream fileStream(filename.c_str(), std::ios::out|std::ios::trunc);
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
  DPRINTF(HWPrefetch, "Record trace contains %i entries to generate %i "
                      "prefetches. Write the trace to %s.\n",
                      entries.size(), num_prefetches, filename);
  fileStream.close();
}












void
IStream::calcStackDistance(Addr addr)
{
  // Align the address to a cache line size
  // const Addr aligned_addr(roundDown(pkt_info.addr, lineSize));
  const Addr aligned_addr(addr);

  // Calculate the stack distance
  const uint64_t sd(sdcalc.calcStackDistAndUpdate(aligned_addr).first);
  if (sd == StackDistCalc::Infinity) {
      // stats.infiniteSD++;
      DPRINTF(HWPrefetch, "InfiniteSD\n");
      return;
  }

  // Sample the stack distance of the address in lin and log bins
  recordStats.hitSDlinHist.sample(sd);
  int sd_lg2 = sd == 0 ? 1 : floorLog2(sd);
  recordStats.hitSDlogHist.sample(sd_lg2);
}




void
IStream::addEventProbeCS(SimObject *obj, const char *name)
{
    warn("Register HWP event probe %s for obj: %s", name, obj->name());
    ProbeManager *pm(obj->getProbeManager());
    listenersSC.push_back(new PrefetchListenerCS(*this, pm, name));
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
  //     return region.first == e._addr;
  //   });
  int hit_idx = 0;
  for (; hit_idx < _buffer.size(); hit_idx++) {
    if (_buffer[hit_idx]->_addr == region.first) {
      break;
    }
  }


  if (hit_idx < _buffer.size()) {
    parent.recordStats.hits++;
    parent.recordStats.bufferHitDistHist.sample(hit_idx);
    // DPRINTF(HWPrefetch, "[%#x] hit: Region (%#x:%u)\n",
    //   addr, _buffer[hit_idx]->_addr, region.second);
  }
  else {
    parent.recordStats.misses++;
    DPRINTF(HWPrefetch, "[%#x] miss. "
      "Add new entry to the record buffer\n", addr);

    // Create new entry and push it into the fifo buffer
    add(region.first);
    hit_idx = 0;
  }

  if (!_buffer[hit_idx]->check(region.second)) {
    parent.pfRecorded++;
  }
  // Mark the entry block in the region as used
  _buffer[hit_idx]->touch(region.second);

  parent.calcStackDistance(region.first);
}

void
  IStream::RecordBuffer::flush(bool all)
{
  // Resize the buffer by writting the last
  // entries into the trace file.
  // Write everything when the all flag is set.
  while (_buffer.size() > bufferEntries || (_buffer.size() && all)) {
    auto wrEntry = _buffer.back();
    auto n_pref = wrEntry->bitsSet();
    float use = usefullness(wrEntry);
    parent.recordStats.totalNumPrefetches += n_pref;

    // TODO parameter
    const float threshold = 0;
    if (use > threshold) {
      parent.recordStats.entryWrites++;
      DPRINTF(HWPrefetch, "Rec. buffer entry %#x:[%x]: write to memory."
        "Contains: %i prefetches -> Usefullness: %.3f\n",
        wrEntry->_addr, wrEntry->_mask, n_pref, use);

      if (!parent.memInterface->writeRecord(wrEntry)) {
        parent.recordStats.entryOverflows++;
      }
      // parent.recordStream->write(wrEntry);
    }
    else {
      parent.recordStats.entryDrops++;
      DPRINTF(HWPrefetch, "Drop buffer entry [%#x:%x] drop. "
                  "Usefullness: %.3f\n",
                   wrEntry->_addr, wrEntry->_mask, use);
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
    if ((*it)->_addr == region.first
      && (*it)->check(region.second)) {
      break;
    }
  }

  if (it == _buffer.end()) {
    return;
  }

  parent.replayStats.notifyHits++;
  // parent.replayStats.bufferHitDistHist.sample(hit_idx);
  // DPRINTF(HWPrefetch, "hit: Region (%#x:%u). Move to front.\n",
  //                   (*it)->_addr, region.second);

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


bool
  IStream::ReplayBuffer::refill()
{
  // In case we are already at the end of the stream we
  // cannot request more entries.
  if (parent.memInterface->replayDone()) {
    return false;
  }
  // DPRINTF(HWPrefetch, "Refill: bsz: %i, pending: %i, entries: %i\n",
  //                         _buffer.size(),pendingFills, bufferEntries);
  while ((_buffer.size() + pendingFills) < bufferEntries) {
    if (!parent.memInterface->readRecord()) {
      DPRINTF(HWPrefetch, "Reached end of trace.\n");
      return false;
    }
    pendingFills++;
  }
  return true;
}

void
  IStream::ReplayBuffer::fill(BufferEntryPtr entry)
{
  _buffer.push_back(entry);
  pendingFills--;
  DPRINTF(HWPrefetch, "Fill: [%s]. size buf.:%i pend. fills %i\n",
                      entry->print(), _buffer.size(), pendingFills);
  parent.replayStats.entryReads++;
}













/*****************************************************************
 * Memory Interface
 * Helper that contains all functionalities to interact with the
 * records in memory
 */
IStream::MemoryInterface::MemoryInterface(IStream& _parent,
                        AddrRange record_addr_range,
                        AddrRange replay_addr_range,
                        PortProxy &_memProxy)
  :
    parent(_parent),
    memProxy(&_memProxy),
    record_addr_range(record_addr_range),
    replay_addr_range(replay_addr_range),
    recordIdx(0), totalRecordEntries(0),
    replayIdx(0), totalReplayEntries(0),
    entry_size(_parent.realEntrySize)
{
  // Before enabling this feature we need to ensure that everything works as
  // intended.
  // assert(maxOutstandingReqs == 0);
  int max_record_entries = record_addr_range.size() / entry_size;
  int max_replay_entries = replay_addr_range.size() / entry_size;
  DPRINTF(HWPrefetch, "Record mem cfg: range: %s"
            " entry size: %iB => capacity: %ikB = %i entries\n",
            record_addr_range.to_string(),
            entry_size, record_addr_range.size()/1024, max_record_entries);
  DPRINTF(HWPrefetch, "Replay mem cfg: range: %s"
            " entry size: %iB => capacity: %ikB = %i entries\n",
            replay_addr_range.to_string(),
            entry_size, replay_addr_range.size()/1024, max_replay_entries);
}


bool
IStream::MemoryInterface::writeRecord(BufferEntryPtr entry)
{
  // Calculate the address in the record memory
  Addr addr = record_addr_range.start() + (recordIdx*entry_size);

  if (!record_addr_range.contains(addr)) {
    warn_once("Record memory space is full. Drop record.\n");
    // DPRINTF(HWPrefetch,"Record memory space is full. Drop record.\n");
    return false;
  }

  // Create a data buffer and write the entry to it.
  // uint8_t* data = new uint8_t[entry_size];
  // entry->writeTo(data);
  uint8_t* data = serialize(entry);

  DPRINTF(HWPrefetch, "WR %#x -> [%s] to MEM. (Nr. %i)\n",
                    addr, entry->print(), totalRecordEntries + 1);

  // Define what happens when the response is received.
  auto completeEvent = new EventFunctionWrapper(
    [this, addr, data] () {
      writeRecordComplete(addr, curTick());
      delete[] data;
    },
    name(),true);

  parent.port.dmaAction(MemCmd::WriteReq, addr, entry_size,
                              completeEvent, data, 0);
  // Entry is written increment record idx
  recordIdx++;
  totalRecordEntries++;
  return true;
}


void
IStream::MemoryInterface::writeRecordComplete(Addr addr, Tick wr_start)
{
  parent.memIFStats.totalWrites++;
  parent.memIFStats.bytesWritten += entry_size;
  parent.memIFStats.totalWriteLatency += curTick() - wr_start;

  DPRINTF(HWPrefetch, "WR %#x complete\n", addr);
}




bool
IStream::MemoryInterface::readRecord()
{
  if (replayIdx >= totalReplayEntries) {
    return false;
  }
  // Calculate the address in the replay memory
  Addr addr = replay_addr_range.start() + (replayIdx*entry_size);

  // Create a data buffer where the data can be written.
  uint8_t* data = new uint8_t[entry_size];

  DPRINTF(HWPrefetch, "RD %#x <- from MEM (%i/%i)\n",
              addr, replayIdx, totalReplayEntries);

  // Define what happens when the response is received.
  auto completeEvent = new EventFunctionWrapper(
    [this, addr, data] () {
      readRecordComplete(addr, data, curTick());
      delete[] data;
    },
    name(),true);

  parent.port.dmaAction(MemCmd::ReadReq, addr, entry_size,
                              completeEvent, data, 0);

  // Entry is requested increment replay idx.
  ++replayIdx;
  return true;
}


void
IStream::MemoryInterface::readRecordComplete(Addr addr,
                                              uint8_t* data, Tick rd_start)
{
  // Upon receiving a read packet from memory we first update the stats.
  parent.memIFStats.totalReads++;
  parent.memIFStats.bytesRead += entry_size;
  parent.memIFStats.totalReadLatency += curTick() - rd_start;

  // // Now create a new buffer entry and copy the data into it.
  // auto newEntry = std::make_shared<BufferEntry>();
  // newEntry->readFrom(data);
  auto newEntry = deserialize(data);


  // delete data;
  // Finally fill the entry in the replay buffer.
  parent.replayBuffer->fill(newEntry);
  DPRINTF(HWPrefetch, "RD %#x -> [%s] complete (%i/%i)\n",
                  addr, newEntry->print(),replayIdx, totalReplayEntries);
}






// bool
// IStream::MemoryInterface::tryGetBackdoor()
// {
//   // We try to get the a backdoor to the trace location in memory
//   // With the first access some dummy data are read.
//   // We do this only once
//   if (bd != nullptr) {
//     // We already have a backdoor pointer
//     return true;
//   }

//   PacketPtr pkt = getPacket(record_addr_range.start(),
//                                 entry_size, MemCmd::ReadReq);
//   parent.backdoor.sendAtomicBackdoor(pkt,bd);
//   delete pkt;

//   if (bd == nullptr) {
//   warn("Cannot create backdoor. We try to use atomic reads/writes instead");
//     return false;
//   }

//   DPRINTF(HWPrefetch, "Created backdoor to memory: [%#x:%#x]\n",
//         bd->range().start(), bd->range().end());

//   // Invalidation callback which finds this backdoor and removes it.
//   auto callback = [this](const MemBackdoor &backdoor) {
//     // This should be the correct backdoor.
//     assert(bd == &backdoor);
//     bd = nullptr;
//   };
//   bd->addInvalidationCallback(callback);
//   return true;
// }





// bool
// IStream::MemoryInterface::readRecordMem(std::vector<BufferEntryPtr>& trace)
// {
//   // Check if the backdoor is connected to the memory
//   if (!parent.backdoor.isConnected())
//   fatal("The backdoor needs to be connected to read the record memory.\n");

//   // Try to get a backdoor to the meta data memory.
//   tryGetBackdoor();

//   // First we will read out the trace from memory.
//   for (int idx = 0; idx < totalRecordEntries; idx++){

//     // Calculate the address in the record memory
//     Addr addr = record_addr_range.start() + (idx*entry_size);
//     assert(record_addr_range.contains(addr));

//     auto entry = std::make_shared<BufferEntry>();


//     if (bd != nullptr) {
//       // Get address in backdoor
//       Addr offset = addr - bd->range().start();
//       entry->readFrom(bd->ptr() + offset);
//     }
//     else {
//       // In case we cannot use the backdoor need to use atomic reads.
//       PacketPtr pkt = getPacket(addr, entry_size, MemCmd::ReadReq);
//       parent.backdoor.sendAtomic(pkt);
//       entry->readFrom(pkt->getPtr<uint8_t>());
//       delete pkt;
//     }

//     // DPRINTF(HWPrefetch, "Read [%s] from address: %#x\n",
//     //             entry->print(), addr);

//     trace.push_back(entry);
//   }

//   DPRINTF(HWPrefetch, "Read %i entries from the record memory.\n",
//             trace.size());
//   return true;
// }






bool
IStream::MemoryInterface::readRecordMem(std::vector<BufferEntryPtr>& entries)
{
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
  for (int idx = 0; idx < totalRecordEntries; idx++){


    // Calculate the address in the record buffer
    int bufferAddr = idx*entry_size;

    // auto entry = std::make_shared<BufferEntry>();
    // entry->readFrom(&buffer[bufferAddr]);
    auto entry = deserialize(&buffer[bufferAddr]);

    DPRINTF(HWPrefetch, "Copy [%s]: from idx: %#x\n",
                entry->print(), bufferAddr);

    entries.push_back(entry);
  }
  delete[] buffer;
  return true;
}





// bool
// IStream::MemoryInterface::initReplayMem(std::vector<BufferEntryPtr>& trace)
// {
//   // Check if the backdoor is connected to the memory
//   if (!parent.backdoor.isConnected())
//    fatal("The backdoor needs to be connected to init the replay memory.\n");

//   // // Try to get a backdoor to the meta data memory.
//   // tryGetBackdoor();


//   Addr addr = replay_addr_range.start();
//   uint64_t size = entry_size * trace.size();
//   if (replay_addr_range.size() < size) {
//         warn("Given address space is not large enough to full with %i en
// tries "
//                 "and cannot hold more entries\n", replayIdx);






//   bool ret_val=true;
//   for (replayIdx = 0; replayIdx < trace.size(); replayIdx++){

//     // Calculate the address in the replay memory
//     Addr addr = replay_addr_range.start() + (replayIdx*entry_size);
//     if (!replay_addr_range.contains(addr)) {
//       warn("Given address space is full with %i entries "
//                       "and cannot hold more entries\n", replayIdx);
//       ret_val = false;
//       break;
//     }

//     if (bd != nullptr) {
//       // Get address in backdoor
//       Addr offset = addr - bd->range().start();
//       trace[replayIdx]->writeTo(bd->ptr() + offset);
//     }
//     else {
//       // In case we cannot use the backdoor need to use atomic reads.
//       PacketPtr pkt = getPacket(addr, entry_size, MemCmd::WriteReq);
//       trace[replayIdx]->writeTo(pkt->getPtr<uint8_t>());
//       parent.backdoor.sendAtomic(pkt);
//       delete pkt;
//     }

//     DPRINTF(HWPrefetch, "Copy [%s]: to address: %#x\n",
//                 trace[replayIdx]->print(), addr);
//   }

//   // Reset the replay pointer to the beginning.
//   totalReplayEntries = replayIdx;
//   replayIdx = 0;

//   DPRINTF(HWPrefetch, "Initialize replay memory with %i entries.\n",
//             totalReplayEntries);

//   return ret_val;
// }

bool
IStream::MemoryInterface::initReplayMem(std::vector<BufferEntryPtr>& trace)
{
  totalReplayEntries = trace.size();

  bool ret_val = true;
  int max_entries = replay_addr_range.size()/entry_size;
  if (totalReplayEntries > max_entries) {
    totalReplayEntries = max_entries;
    warn("Given address space is not large enough. "
          "Will only write the first %i entries.\n", totalReplayEntries);
    ret_val = false;
  }

  // Prepare a buffer
  uint64_t size = totalReplayEntries * entry_size;
  uint8_t* buffer = new uint8_t[size];

  // Write the initialisation entries to it.
  for (int idx = 0; idx < totalReplayEntries; idx++){
    // Calculate the address in the replay memory
    int bufferAddr = idx*entry_size;

    // trace[idx]->writeTo(&buffer[bufferAddr]);
    serialize(trace[idx], &buffer[bufferAddr]);

    DPRINTF(HWPrefetch, "Copy [%s]: to address: %#x\n",
                trace[idx]->print(), bufferAddr);
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



// void
// IStream::MemoryInterface::initMem(Addr addr, uint8_t *data, uint64_t size)
// {

// }
// void
// IStream::MemoryInterface::initMemComplete()
// {

// }











// PacketPtr
// IStream::MemoryInterface::getPacket(Addr addr, unsigned size,
//                           const MemCmd& cmd, Request::FlagsType flags)
// {
//   // Create new request
//   RequestPtr req = std::make_shared<Request>(addr, size, flags,
//                                               parent.requestorId);
//   // Dummy PC to have PC-based prefetchers latch on; get entropy into higher
//   // bits
//   req->setPC(((Addr)parent.requestorId) << 2);

//   // Embed it in a packet
//   PacketPtr pkt = new Packet(req, cmd);

//   uint8_t* pkt_data = new uint8_t[req->getSize()];
//   pkt->dataDynamic(pkt_data);

//   if (cmd.isWrite()) {
//       std::fill_n(pkt_data, req->getSize(), (uint8_t)parent.requestorId);
//   }
//   parent.memIFStats.numPackets++;
//   return pkt;
// }


// bool
// IStream::MemoryInterface::sendPacket(PacketPtr pkt, bool atomic)
// {
//   DPRINTF(HWPrefetch, "%s %#x -> send request. \n",
//           pkt->isWrite() ? "WR" : "RD", pkt->req->getPaddr());

//   // Try to send the packet.
//   // Only attempts to send if not blocked by pending responses
//   blockedWaitingResp = allocateWaitingRespSlot(pkt);

//   // For atomic mode we send and the packet and receive the response
//   // immediately
//   if (atomic) {
//   // if (true) {
//     // Send the packet.
//     parent.port.sendAtomic(pkt);
//     recvTimingResp(pkt);
//     return true;
//   }

//   if (// In case there are already waiting packets we will not try now.
//       // Just enqueue the packet. It will be send later.
//       blockedPkts.size() > 0
//       // we will also not try in case the there are we are blocked on
//       // waiting resp capacity limit.
//       || blockedWaitingResp
//      // In case the attempt fail we enqueue the packet in the blocked queue.
//       || !parent.port.sendTimingReq(pkt)){

//     blockedPkts.push_back(pkt);
//     DPRINTF(HWPrefetch, "Cannot send now. Try later: %i packets "
//                   "already blocked for sending.\n",
//                   blockedPkts.size());
//     // In case the retry tick was not set we will do it now.
//     if (retryPktTick == 0) {
//       retryPktTick = curTick();
//     }
//     return false;
//   }
//   return true;

//   // parent.dmaPort

// }


// // void
// // CntLogic::read(Addr addr, uint8_t* data, uint64_t size,
// //         std::function<void(void)> callback)
// // {
// //     // Callback will install the line in the cache although it has not
// //     // been yet verified by the counter logic (unverifiedAddresses takes
// //     // care of this)
// //     counterPort.dmaAction(MemCmd::ReadReq, addr, size,
// //     new EventFunctionWrapper(callback, "Read counter from memory", true),
// //         data, 0);
// // }

// // void
// // CntLogic::write(Addr addr, uint8_t* data, uint64_t size,
// //         std::function<void(void)> callback)
// // {
// //     const ChannelAddr ch_addr(chRange, addr);
// //     handleEviction(ProbeEvictionReturn(ch_addr, [=](int counter) {
// //         counterPort.dmaAction(
// //             MemCmd::WriteReq, addr, size,
// //          new EventFunctionWrapper(callback, "CCL written back to memory",
// //                     true),
// //             data, macLatency + queues.hashAndAes.getWaitAndInc());
// //     }));
// // }






// bool
// IStream::MemoryInterface::allocateWaitingRespSlot(PacketPtr pkt)
// {
//   assert(waitingResp.find(pkt->req) == waitingResp.end());
//   assert(pkt->needsResponse());

//   waitingResp[pkt->req] = curTick();

//   return (maxOutstandingReqs > 0) &&
//         (waitingResp.size() > maxOutstandingReqs);
// }


// // void
// // IStream::MemoryInterface::recvReqRetry()
// // {

// //   retryReq();
// // }

// void
// IStream::MemoryInterface::retryReq()
// {
//   DPRINTF(HWPrefetch, "Received retry\n");
//   assert(blockedPkts.size() > 0);

//   // Attempt to send the first packet in the blocked queue.
//   // Because this was the one causing the blocking.
//   // We might get blocked again.
//   auto retryPkt = blockedPkts.front();
//   parent.memIFStats.numRetries++;

//   // Since the port notify that its now ready again to receive new requests
//   // the first request should never fail
//   assert(parent.port.sendTimingReq(retryPkt));
//   // if () {
//   //   DPRINTF(HWPrefetch, "%s %#x Fail again to resend. \n",
//   //       retryPkt->isWrite() ? "WR" : "RD", retryPkt->req->getPaddr());
//   //   return;
//   // }
//   DPRINTF(HWPrefetch, "%s %#x retry successfull. \n",
//         retryPkt->isWrite() ? "WR" : "RD", retryPkt->req->getPaddr());

//   // We can remove the packet from the blocking queue.
//   blockedPkts.pop_front();

//   // In there are more packets blocked we will try to send one more packet.
//   // This one will fail again but cases that the port inform us when its
//   // free again.
//   if (blockedPkts.size() > 0) {
//     DPRINTF(HWPrefetch, "Still %i packets are waiting to be send. "
//                     "Blocked again.\n", blockedPkts.size());
//     retryPkt = blockedPkts.front();
//     // The second request should fail.
//     assert(!parent.port.sendTimingReq(retryPkt));
//     return;
//   }

//   DPRINTF(HWPrefetch, "No more blocked packets waiting \n");
//   // remember how much delay was incurred due to back-pressure
//   // when sending the request
//   Tick delay = curTick() - retryPktTick;
//   retryPktTick = 0;
//   parent.memIFStats.retryTicks += delay;
// }


// bool
// IStream::MemoryInterface::recvTimingResp(PacketPtr pkt)
// {
//   auto iter = waitingResp.find(pkt->req);

//   panic_if(iter == waitingResp.end(), "%s: "
//           "Received unexpected response [%s reqPtr=%x]\n",
//               pkt->print(), pkt->req);
//   assert(iter->second <= curTick());

//   // Handle Write response ----
//   if (pkt->isWrite()) {
//     // For a write only the stats need to be updated.
//     parent.memIFStats.totalWrites++;
//     parent.memIFStats.bytesWritten += pkt->req->getSize();
//     parent.memIFStats.totalWriteLatency += curTick() - iter->second;

//     DPRINTF(HWPrefetch, "WR %#x complete\n", pkt->req->getPaddr());
//   }

//   // Handle Read response ----
//   if (pkt->isRead()) {
//     // Upon receiving a read packet from memory we first update the stats.
//     parent.memIFStats.totalReads++;
//     parent.memIFStats.bytesRead += pkt->req->getSize();
//     parent.memIFStats.totalReadLatency += curTick() - iter->second;

//     // Now create a new buffer entry and copy the data into it.
//     auto newEntry = std::make_shared<BufferEntry>();
//     newEntry->readFrom(pkt->getPtr<uint8_t>());
//     parent.replayBuffer->fill(newEntry);

//     DPRINTF(HWPrefetch, "RD %#x -> [%s] complete\n",
//                             pkt->req->getPaddr(), newEntry->print());
//   }

//   // Delete the response from the waiting list and the packet
//   waitingResp.erase(iter);
//   delete pkt;

//   // Retry to send blocked packets in case there are some.
//   // if (blockedPkts.size() > 0) {
//   //   retryReq();
//   // }

//   //
//   // if (blockedWaitingResp) {
//   //   blockedWaitingResp = false;
//   //   retryReq();
//   // }
//   return true;
// }

DrainState
IStream::MemoryInterface::drain()
{
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

Port &
IStream::getPort(const std::string &if_name, PortID idx)
{
  if (if_name == "port") {
      return port;
  }
  return ClockedObject::getPort(if_name, idx);
}

// IStream::IStreamMemPort::IStreamMemPort(
//             const std::string &_name, IStream &_parent)
//   : RequestPort(_name, &_parent), parent(_parent)
// {}

// IStream::IStreamMemBDPort::IStreamMemBDPort(
//             const std::string &_name, IStream &_parent)
//   : RequestPort(_name, &_parent), parent(_parent)
// {}

// bool
// IStream::trySatisfyFunctional(PacketPtr pkt)
// {
//   // return port.trySatisfyFunctional(pkt);
// }

















    // IStream::TraceStream::TraceStream(const std::string& filename) :
    // fileStream(filename.c_str(), std::ios::in|std::ios::out|std::ios::trunc)
    // {
    //   if (!fileStream.good())
    //     panic("Could not open %s\n", filename);
    // }

    // IStream::TraceStream::~TraceStream()
    // {
    //   // As the compression is optional, see if the stream exists
    //   // if (gzipStream != NULL)
    //   //     delete gzipStream;
    //   // delete wrappedFileStream;
    //   fileStream.close();
    // }

    // void
    //   IStream::TraceStream::write(BufferEntry* entry)
    // {
    //   // DPRINTF(HWPrefetch, "WR: %s\n",entry->print());
    //   // std::getline(sstream, record);
    //   // fileStream << entry->_addr << "," << entry->_mask << "\n";

    //   fileStream.write((char*)entry, sizeof(*entry));
    // }

    // bool
    //   IStream::TraceStream::read(BufferEntry* entry)
    // {
    //   if (!fileStream.eof()) {
    //     fileStream.read((char*)entry, sizeof(*entry));

    //     // std::string record;
    //     // std::getline(fileStream, record);
    //     // std::istringstream line(record);
    //     // if (fileStream.eof()) {
    //     //   return false;
    //     // }

    //     // std::getline(line, record, ',');
    //     // entry->_addr = Addr(std::stoull(record));
    //     // std::getline(line, record, ',');
    //     // entry->_mask = std::stoull(record);

    //     DPRINTF(HWPrefetch, "RD: %s\n",entry->print());
    //     return true;
    //   }
    //   return false;
    // }
    // void
    //   IStream::TraceStream::reset()
    // {
    //   // seek to the start of the input file and clear any flags
    //   fileStream.clear();
    //   fileStream.seekg(0, std::ifstream::beg);
    // }






IStream::IStreamRecStats::IStreamRecStats(IStream* parent,
  const std::string& name)
  : statistics::Group(parent, name.c_str()),
  ADD_STAT(notifies, statistics::units::Count::get(),
    "Number of notification accesses the recording got."),
  ADD_STAT(hits, statistics::units::Count::get(),
    "Number of hits in the record buffer"),
  ADD_STAT(misses, statistics::units::Count::get(),
    "Number of misses in the record buffer"),
  ADD_STAT(cacheHit, statistics::units::Count::get(),
    "Number of accesses we might skip recording because it is a cache hit"),
  ADD_STAT(inCache, statistics::units::Count::get(),
    "Number of accesses we might skip recording because pAddr is in cache"),
  ADD_STAT(hitInMissQueue, statistics::units::Count::get(),
    "Number of accesses we skipped for recording because addr in missqueue"),
  ADD_STAT(cacheHitBecausePrefetch, statistics::units::Count::get(),
    "Number of cache hits not skipped because they where useful cache hits."),
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
    "Average number of prefetches within one entry."),
  ADD_STAT(hitSDlinHist, statistics::units::Count::get(),
               "Hit stack distance linear distribution"),
  ADD_STAT(hitSDlogHist, statistics::units::Ratio::get(),
               "Hit stack distance logarithmic distribution")
{
  using namespace statistics;

  const IStreamPrefetcherParams& p =
    dynamic_cast<const IStreamPrefetcherParams&>(parent->params());

  bufferHitDistHist
    .init((p.record_buffer_entries < 2) ? 2
       : ((p.record_buffer_entries > 16) ? 16 : p.record_buffer_entries))
    .flags(pdf);

  avgNumPrefetchesPerEntry.flags(total);
  avgNumPrefetchesPerEntry = totalNumPrefetches / entryWrites;

  hitSDlinHist
      .init(16)
      .flags(pdf);

  hitSDlogHist
      .init(32)
      .flags(pdf);

}

IStream::IStreamReplStats::IStreamReplStats(IStream* parent,
  const std::string& name)
  : statistics::Group(parent, name.c_str()),
  ADD_STAT(notifyHits, statistics::units::Count::get(),
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
  ADD_STAT(pfBufferHit, statistics::units::Count::get(),
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
    "Average usefullness of the entries read from the trace.")
{
  using namespace statistics;

  // const IStreamPrefetcherParams& p =
  //   dynamic_cast<const IStreamPrefetcherParams&>(parent->params());
  pfDrops.flags(total);
  pfDrops = pfBufferHit + pfHitInCache + pfTranslationFail;

  accuracy.flags(total);
  accuracy = pfUseful / pfInsertedInQueue;

  coverage.flags(total);
  coverage = pfUseful / (pfUseful + parent->prefetchStats.demandMshrMisses);

  avgPrefetchesPerEntry.flags(total);
  avgPrefetchesPerEntry = pfGenerated / entryReads;

}

IStream::IStreamMemIFStats::IStreamMemIFStats(IStream* parent,
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
    ADD_STAT(totalReadLatency, statistics::units::Tick::get(),
              "Total latency of read requests"),
    ADD_STAT(totalWriteLatency, statistics::units::Tick::get(),
              "Total latency of write requests"),
    ADD_STAT(totalReads, statistics::units::Count::get(),
              "Total num of reads"),
    ADD_STAT(totalWrites, statistics::units::Count::get(),
              "Total num of writes"),
    ADD_STAT(avgReadLatency, statistics::units::Rate<
                  statistics::units::Tick, statistics::units::Count>::get(),
              "Avg latency of read requests", totalReadLatency / totalReads),
    ADD_STAT(avgWriteLatency, statistics::units::Rate<
                  statistics::units::Tick, statistics::units::Count>::get(),
              "Avg latency of write requests",
              totalWriteLatency / totalWrites),
    ADD_STAT(readBW, statistics::units::Rate<
                  statistics::units::Byte, statistics::units::Second>::get(),
              "Read bandwidth", bytesRead / simSeconds),
    ADD_STAT(writeBW, statistics::units::Rate<
                  statistics::units::Byte, statistics::units::Second>::get(),
              "Write bandwidth", bytesWritten / simSeconds)
{
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
