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
  memInterface(nullptr),
  port(name() + "-port", *this),
  degree(p.degree),
  regionSize(p.region_size),

  // recordTrace(p.record_trace),
  // replayTrace(p.replay_trace),

  recordStats(this, "recordStats"),
  replayStats(this, "replayStats")
{
  lRegionSize = floorLog2(regionSize);
  blksPerRegion = regionSize / blkSize;
  DPRINTF(HWPrefetch, "Cfg: region size: %i, blk size %i => "
                "%i blocks per region.\n", regionSize, blkSize,
                    blksPerRegion);


  recordStream = new TraceStream(p.trace_record_file);
  replayStream = new TraceStream(p.trace_replay_file);
  memInterface = new MemoryInterface(*this,
                    p.record_addr_range,
                    p.replay_addr_range,
                    p.max_outstanding_reqs);
  recordBuffer = new RecordBuffer(*this, p.buffer_entries, regionSize);
  replayBuffer = new ReplayBuffer(*this, p.buffer_entries, regionSize);




  // Register a callback to compensate for the destructor not
  // being called. The callback forces the stream to flush and
  // closes the output file.
  registerExitCallback([this]() { closeStreams(); });
}

void
IStream::init()
{
    if (!port.isConnected())
        fatal("IStream prefetcher need to be connected to memory.\n");
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

      recordBuffer->access(blkAddr);
      recordBuffer->flush();

    }

    void
      IStream::replay(const PrefetchInfo& pfi,
        std::vector<AddrPriority>& addresses)
    {
      // TODO: Fill up timely
      replayBuffer->refill();
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
  DPRINTF(HWPrefetch, "Start Recoding instructions\n");
  recordStream->reset();
  recordBuffer->clear();
  memInterface->reset();
}


void
  IStream::initReplay(std::string filename)
{
  std::fstream fileStream(filename.c_str(), std::ios::in);
  if (!fileStream.good()) {
    panic("Could not open %s\n", filename);
    return;
  }

  std::vector<std::shared_ptr<BufferEntry>> trace;

  while (!fileStream.eof()) {
    // File was in binary format.
    // fileStream.read((char*)entry, sizeof(*entry));

    // File was in human readable format.
    auto tmp = std::make_shared<BufferEntry>();
    if (!tmp->parseLine(fileStream)) {
      break;
    }
    trace.push_back(tmp);
  }

  fileStream.close();

  DPRINTF(HWPrefetch, "Read %i entries from %s: Init mem.\n",
                trace.size(), filename);
  // Write the entries to memory
  memInterface->initReplayMem(trace);
  // After writting the stream
  // set the stream pointer to the start again.
  replayStream->reset();
  replayBuffer->clear();
  memInterface->reset();
}

void
  IStream::dumpRecTrace(std::string filename)
{
  std::fstream fileStream(filename.c_str(),
                            std::ios::out|std::ios::trunc);
  // Make sure the content is fully written to memory.
  recordBuffer->flush(true);

  std::vector<BufferEntryPtr> trace;
  memInterface->readRecordMem(trace);

  DPRINTF(HWPrefetch, "Got %i entries from memory\n", trace.size());
  for (auto it : trace) {
    fileStream << it->toReadable() << "\n";
  }
  fileStream.close();
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
    DPRINTF(HWPrefetch, "[%#x] hit: Region (%#x:%u)\n",
      addr, _buffer[hit_idx]->_addr, region.second);
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
  IStream::RecordBuffer::flush(bool all)
{
  // Resize the buffer by writting the last
  // entries into the trace file.
  // Write everything when the all flag is set.
  while (_buffer.size() > bufferEntries || (_buffer.size() && all)) {
    auto wrEntry = _buffer.back();
    float use = usefullness(wrEntry);
    parent.recordStats.cumUsefullness += use;

    // TODO parameter
    const float threshold = 0;
    if (use > threshold) {
      parent.recordStats.entryWrites++;
      DPRINTF(HWPrefetch, "Rec. buffer entry %#x:[%x]: write to memory."
        " Usefullness: %.3f\n", wrEntry->_addr, wrEntry->_mask, use);

      if (!parent.memInterface->writeRecord(wrEntry, all ? true : false)) {
        // if (!parent.memInterface->writeRecord(wrEntry, true)) {
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

  parent.replayStats.hits++;
  parent.replayStats.bufferHitDistHist.sample(hit_idx);
  DPRINTF(HWPrefetch, "hit: Region (%#x:%u). Move to front.\n",
    (*it)->_addr, region.second);

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

void
  IStream::ReplayBuffer::refill()
{
  // In case we are already at the end of the stream we
  // cannot request more entries.
  if (eof) {
    return;
  }
  while (_buffer.size() < (bufferEntries + pendingFills)) {
    // if (!parent.memInterface->getNextRecord(true)) {
    if (!parent.memInterface->getNextRecord()) {
      DPRINTF(HWPrefetch, "Reached end of file.\n");
      eof = true;
      break;
    }
    pendingFills++;
  }
}


void
  IStream::ReplayBuffer::fill(BufferEntryPtr entry)
{
  DPRINTF(HWPrefetch, "Fill: %s\n", entry->print());
  _buffer.push_back(entry);
  pendingFills--;
}













/*****************************************************************
 * Memory Interface
 * Helper that contains all functionalities to interact with the
 * records in memory
 */
IStream::MemoryInterface::MemoryInterface(IStream& _parent,
                        AddrRange record_addr_range,
                        AddrRange replay_addr_range,
                        int max_outstanding_reqs):
  parent(_parent),
  // port(name() + "-port", *this),
  // reqQueue(*this, port),
  // snoopRespQueue(*this, port),

  record_addr_range(record_addr_range),
  replay_addr_range(replay_addr_range),
  recordIdx(0), totalRecordEntries(0),
  replayIdx(0), totalReplayEntries(0),
  entry_size(2*sizeof(uint64_t)),
  retryPkt(NULL),
  retryPktTick(0), blockedWaitingResp(false),
  maxOutstandingReqs(max_outstanding_reqs)
{
  // Before enabling this feature we need to ensure that everything works as
  // intended.
  assert(maxOutstandingReqs == 0);
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
IStream::MemoryInterface::writeRecord(BufferEntryPtr entry, bool atomic)
{
  // Calculate the address in the record memory
  Addr addr = record_addr_range.start() + (recordIdx*entry_size);

  if (!record_addr_range.contains(addr)) {
    DPRINTF(HWPrefetch,"Record memory space is full. Drop record.\n");
    return false;
  }

  // Create a packet and write the entry data to it.
  PacketPtr pkt = getPacket(addr, entry_size, MemCmd::WriteReq);
  entry->writeTo(pkt->getPtr<uint8_t>());

  // try to send the packet. We might fail to send the packet
  // right away but then the packet gets appended to the blocking queue
  bool success = sendPacket(pkt,atomic);

  // Entry is written increment record idx
  recordIdx++;
  totalRecordEntries++;
  return true;
}


bool
IStream::MemoryInterface::getNextRecord(bool atomic)
{
  if (replayIdx >= totalReplayEntries) {
    return false;
  }
  // Calculate the address in the replay memory
  Addr addr = replay_addr_range.start() + (replayIdx*entry_size);

  // Create a packet.
  PacketPtr pkt = getPacket(addr, entry_size, MemCmd::ReadReq);
  sendPacket(pkt,atomic);

  // Entry is written increment replay idx.
  replayIdx++;
  return true;
}


bool
IStream::MemoryInterface::sendPacket(PacketPtr pkt, bool atomic)
{
  DPRINTF(HWPrefetch, "%s %#x -> send request. \n",
          pkt->isWrite() ? "WR" : "RD", pkt->req->getPaddr());

  // Try to send the packet.
  // Only attempts to send if not blocked by pending responses
  blockedWaitingResp = allocateWaitingRespSlot(pkt);

  // For atomic mode we send and the packet and receive the response
  // immediately
  // if (atomic) {
  if (true) {
    // Send the packet.
    parent.port.sendAtomic(pkt);
    recvTimingResp(pkt);
  }

  // In timing mode we send the packet but we will get the reponse
  // asyncron later.
  else {
    assert(false); // Not implemented yet.
    if (// In case there are already waiting packets we will not try now.
        // Just enqueue the packet. It will be send later.
        blockedPkts.size() > 0
        // we will also not try in case the there are we are blocked on
        // waiting resp capacity limit.
        || blockedWaitingResp
        // In case the attempt fail we enqueue the packet in the blocked queue.
        || !parent.port.sendTimingReq(pkt)){

      blockedPkts.push_back(pkt);
      DPRINTF(HWPrefetch, "Cannot send now. Try later: %i packets "
                    "already blocked for sending.\n",
                    blockedPkts.size() > 0);
      Tick retryPktTick;
      return false;
    }
  }
  return true;
}

bool
IStream::MemoryInterface::allocateWaitingRespSlot(PacketPtr pkt)
{
  assert(waitingResp.find(pkt->req) == waitingResp.end());
  assert(pkt->needsResponse());

  waitingResp[pkt->req] = curTick();

  return (maxOutstandingReqs > 0) &&
        (waitingResp.size() > maxOutstandingReqs);
}

bool
IStream::MemoryInterface::readRecordMem(std::vector<BufferEntryPtr>& trace)
{
  // We try to get the a backdoor to the trace location in memory
  // With the first access some dummy data are read.
  PacketPtr pkt = getPacket(replay_addr_range.start(),
                                entry_size, MemCmd::ReadReq);
  MemBackdoorPtr bd = nullptr;
  parent.port.sendAtomicBackdoor(pkt,bd);
  delete pkt;

  if (bd == nullptr) {
    warn("Cannot create backdoor. Trace cannot be read from memory.");
    return false;
  }
  DPRINTF(HWPrefetch, "Created backdoor to memory: [%#x:%#x]\n",
            bd->range().start(), bd->range().end());


  for (int idx = 0; idx < totalRecordEntries; idx++){

    // Calculate the address in the record memory
    Addr addr = record_addr_range.start() + (idx*entry_size);
    assert(record_addr_range.contains(addr));

    auto entry = std::make_shared<BufferEntry>();

    // Get address in backdoor
    Addr offset = addr - bd->range().start();
    entry->readFrom(bd->ptr() + offset);

    // DPRINTF(HWPrefetch, "Read [%s] from address: %#x\n",
    //             entry->print(), addr);

    trace.push_back(entry);
  }

  DPRINTF(HWPrefetch, "Read %i entries from the record memory.\n",
            trace.size());
  return true;
}


bool
IStream::MemoryInterface::initReplayMem(std::vector<BufferEntryPtr>& trace)
{
  // We try to the a backdoor to the trace location in memory
  // With the first access some dummy data are written but we do not
  // care because we overwrite them anyway later.
  PacketPtr pkt = getPacket(replay_addr_range.start(),
                              entry_size, MemCmd::WriteReq);
  MemBackdoorPtr bd = nullptr;
  parent.port.sendAtomicBackdoor(pkt,bd);
  delete pkt;

  if (bd == nullptr) {
    warn("Cannot create backdoor. Trace cannot be initialized.");
    return false;
  }
  DPRINTF(HWPrefetch, "Created backdoor to memory: [%#x:%#x]\n",
            bd->range().start(), bd->range().end());

  for (replayIdx = 0; replayIdx < trace.size(); replayIdx++){

    // Calculate the address in the replay memory
    Addr addr = replay_addr_range.start() + (replayIdx*entry_size);
    if (!replay_addr_range.contains(addr)) {
      warn("Given address space is full with %i entries "
                      "and cannot hold more entries\n", replayIdx);
      break;
    }

    // Get address in backdoor
    Addr offset = addr - bd->range().start();
    trace[replayIdx]->writeTo(bd->ptr() + offset);

    DPRINTF(HWPrefetch, "Copy [%s]: to address: %#x\n",
                trace[replayIdx]->print(), addr);
  }

  // Reset the replay pointer to the beginning.
  totalReplayEntries = replayIdx;
  replayIdx = 0;

  DPRINTF(HWPrefetch, "Initialize replay memory with %i entries.\n",
            totalReplayEntries);

  return true;
}








PacketPtr
IStream::MemoryInterface::getPacket(Addr addr, unsigned size,
                          const MemCmd& cmd, Request::FlagsType flags)
{
    // Create new request
    RequestPtr req = std::make_shared<Request>(addr, size, flags,
                                               parent.requestorId);
    // Dummy PC to have PC-based prefetchers latch on; get entropy into higher
    // bits
    req->setPC(((Addr)parent.requestorId) << 2);

    // Embed it in a packet
    PacketPtr pkt = new Packet(req, cmd);

    uint8_t* pkt_data = new uint8_t[req->getSize()];
    pkt->dataDynamic(pkt_data);

    if (cmd.isWrite()) {
        std::fill_n(pkt_data, req->getSize(), (uint8_t)parent.requestorId);
    }

    return pkt;
}


void
IStream::MemoryInterface::recvReqRetry()
{
    DPRINTF(HWPrefetch, "Received retry\n");
    retryReq();
}

void
IStream::MemoryInterface::retryReq()
{
  assert(retryPkt != NULL);
  assert(retryPktTick != 0);
  assert(!blockedWaitingResp);

  auto wr = retryPkt->isWrite();
  if (wr) {
    parent.recordStats.numRetries++;
  } else {
    parent.replayStats.numRetries++;
  }

  // attempt to send the packet, and if we are successful start up
  // the machinery again
  if (parent.port.sendTimingReq(retryPkt)) {
    retryPkt = NULL;
    // remember how much delay was incurred due to back-pressure
    // when sending the request, we also use this to derive
    // the tick for the next packet
    Tick delay = curTick() - retryPktTick;
    retryPktTick = 0;
    if (wr) {
      parent.recordStats.retryTicks += delay;
    } else {
      parent.replayStats.retryTicks += delay;
    }
  }
}


bool
IStream::MemoryInterface::recvTimingResp(PacketPtr pkt)
{
  auto iter = waitingResp.find(pkt->req);

  panic_if(iter == waitingResp.end(), "%s: "
          "Received unexpected response [%s reqPtr=%x]\n",
              pkt->print(), pkt->req);
  assert(iter->second <= curTick());

  // Handle Write response ----
  if (pkt->isWrite()) {
    // For a write only the stats need to be updated.
    parent.recordStats.totalWrites++;
    parent.recordStats.bytesWritten += pkt->req->getSize();
    parent.recordStats.totalWriteLatency += curTick() - iter->second;
    DPRINTF(HWPrefetch, "WR %#x complete\n", pkt->req->getPaddr());
  }

  // Handle Read response ----
  if (pkt->isRead()) {
    // Upon receiving a read packet from memory we first update the stats.
    parent.replayStats.totalReads++;
    parent.replayStats.bytesRead += pkt->req->getSize();
    parent.replayStats.totalReadLatency += curTick() - iter->second;

    // Now create a new buffer entry and copy the data into it.
    auto newEntry = std::make_shared<BufferEntry>();
    newEntry->readFrom(pkt->getPtr<uint8_t>());
    parent.replayBuffer->fill(newEntry);
    DPRINTF(HWPrefetch, "RD %#x -> [%s] complete\n",
                            pkt->req->getPaddr(), newEntry->print());
  }

  // Delete the response from the waiting list and the packet
  waitingResp.erase(iter);
  delete pkt;

  // Sends up the request if we were blocked
  if (blockedWaitingResp) {
    blockedWaitingResp = false;
    retryReq();
  }
  return true;
}

Port &
IStream::getPort(const std::string &if_name, PortID idx)
{
  return port;
}

IStream::IStreamMemPort::IStreamMemPort(
            const std::string &_name, IStream &_parent)
  : RequestPort(_name, &_parent), parent(_parent)
{}


// bool
// IStream::trySatisfyFunctional(PacketPtr pkt)
// {
//   // return port.trySatisfyFunctional(pkt);
// }

















    IStream::TraceStream::TraceStream(const std::string& filename) :
      fileStream(filename.c_str(), std::ios::in|std::ios::out|std::ios::trunc)
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
      // fileStream << entry->_addr << "," << entry->_mask << "\n";

      fileStream.write((char*)entry, sizeof(*entry));
    }

    bool
      IStream::TraceStream::read(BufferEntry* entry)
    {
      if (!fileStream.eof()) {
        fileStream.read((char*)entry, sizeof(*entry));

        // std::string record;
        // std::getline(fileStream, record);
        // std::istringstream line(record);
        // if (fileStream.eof()) {
        //   return false;
        // }

        // std::getline(line, record, ',');
        // entry->_addr = Addr(std::stoull(record));
        // std::getline(line, record, ',');
        // entry->_mask = std::stoull(record);

        DPRINTF(HWPrefetch, "RD: %s\n",entry->print());
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
  ADD_STAT(entryOverflows, statistics::units::Count::get(),
    "Number of entires not be write to the record because of "
    "limited memory capacity."),
  ADD_STAT(cumUsefullness, statistics::units::Count::get(),
    "Cumulative usefullness of the entries that fall out of the "
    "fifo buffer"),
  ADD_STAT(avgUsefullness, statistics::units::Count::get(),
    "Average usefullness of the entries that fall out of the fifo buffer"),

  ADD_STAT(totalWrites, statistics::units::Count::get(),
            "Total num of writes"),
  ADD_STAT(numRetries, statistics::units::Count::get(), "Number of retries"),
  ADD_STAT(retryTicks, statistics::units::Tick::get(),
            "Time spent waiting due to back-pressure"),
  ADD_STAT(bytesWritten, statistics::units::Byte::get(),
            "Number of bytes written"),
  ADD_STAT(totalWriteLatency, statistics::units::Tick::get(),
            "Total latency of write requests"),
  ADD_STAT(avgWriteLatency, statistics::units::Rate<
                statistics::units::Tick, statistics::units::Count>::get(),
            "Avg latency of write requests",
            totalWriteLatency / totalWrites),
  ADD_STAT(writeBW, statistics::units::Rate<
                statistics::units::Byte, statistics::units::Second>::get(),
            "Write bandwidth", bytesWritten / simSeconds)
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
    "Average usefullness of the entries that fall out of the fifo buffer"),

  ADD_STAT(totalReads, statistics::units::Count::get(), "Total num of reads"),
  ADD_STAT(numRetries, statistics::units::Count::get(), "Number of retries"),
  ADD_STAT(retryTicks, statistics::units::Tick::get(),
            "Time spent waiting due to back-pressure"),
  ADD_STAT(bytesRead, statistics::units::Byte::get(), "Number of bytes read"),
  ADD_STAT(totalReadLatency, statistics::units::Tick::get(),
            "Total latency of read requests"),
  ADD_STAT(avgReadLatency, statistics::units::Rate<
                statistics::units::Tick, statistics::units::Count>::get(),
            "Avg latency of read requests", totalReadLatency / totalReads),
  ADD_STAT(readBW, statistics::units::Rate<
                statistics::units::Byte, statistics::units::Second>::get(),
            "Read bandwidth", bytesRead / simSeconds)
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
