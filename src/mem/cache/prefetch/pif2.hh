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
 * Describes the PIF prefetcher as in the Champsim implementation.
 */

#ifndef __MEM_CACHE_PREFETCH_PIF2_HH__
#define __MEM_CACHE_PREFETCH_PIF2_HH__

// #include "mem/cache/prefetch/queued.hh"
// #include "mem/packet.hh"

// namespace gem5
// {

// struct FakePrefetcherParams;

// GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
// namespace prefetch
// {

// class Fake : public Queued
// {
//   protected:
//       const int degree;

//   public:
//     Fake(const FakePrefetcherParams &p);
//     ~Fake() = default;

//     void calculatePrefetch(const PrefetchInfo &pfi,
//                          std::vector<AddrPriority> &addresses) override;
// };

// } // namespace prefetch
// } // namespace gem5

#include <algorithm>
#include <bitset>
#include <list>
#include <map>
#include <vector>

#include "mem/cache/prefetch/base.hh"

namespace gem5
{

struct PIF2PrefetcherParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch

{
using namespace std;

class PIF2 : public Base
{
private:

    ////////////////////////////////////////////////////////////
    // PIF from champsim
    //////////////////////////////////
    #define _regionSize 8
    #define _spatialOffset 0 // posof the first block access in a region
    #define _theLookahead 5
    #define _theTrackerSize 12
    #define _theCompactorSize 18 // 6;
    #define _theStreamCount 3
    #define _theBlockSize 64
    #define _theBlockOffsetSize 6 // (int)log2(theBlockSize)
    #define _theBlockMask (~(_theBlockSize - 1ULL))

    /** Cycles after generation when a prefetch can first be issued */
    const Cycles latency;


    const int regionSize = _regionSize;
    // position of the first block accessed in a region
    const int spatialOffset = _spatialOffset;
    const int theLookahead = _theLookahead;
    const int theTrackerSize = _theTrackerSize;
    const int theCompactorSize = _theCompactorSize; // 6;
    const int theStreamCount = _theStreamCount;
    const int theBlockSize = _theBlockSize;
    const int theBlockOffsetSize = (int)log2(theBlockSize);
    uint64_t theBlockMask = ~(theBlockSize - 1);

    const int infinite;
    const bool cacheSnoop;
    Tick next_ready_time;

    #define PIF_INF

    struct PFQEntry
    {
        PFQEntry(uint64_t _addr)
            : addr(_addr), pkt(nullptr), readyTime(MaxTick) {}
        PFQEntry(uint64_t _addr, Tick t)
            : addr(_addr), pkt(nullptr), readyTime(t) {}
        PFQEntry(uint64_t _addr, PacketPtr p, Tick t)
            : addr(_addr), pkt(p), readyTime(t) {}
        uint64_t addr;
        PacketPtr pkt;
        Tick readyTime;
        bool operator==(const int& a) const {
            return this->addr == a;
        }
    };



    struct IndexEntry
    {
        uint64_t key;
        uint64_t value;

        IndexEntry()
        {
            key = 0;
            value = 0;
        };

        IndexEntry(uint64_t k, uint64_t v)
        {
            key = k;
            value = v;
        };
    };

    struct Index
    {
        int theAssoc;
        int theSets;

        uint64_t theSetMask;

        typedef map<uint64_t, list<IndexEntry *>> IndexTable;
        typedef map<uint64_t, list<IndexEntry *>>::iterator IndexIter;

        typedef list<IndexEntry *>::iterator SetIter;

        IndexTable theIndex;

        Index(int sets, int assoc)
        {
            theSets = sets;
            theAssoc = assoc;
            theSetMask = sets - 1;
            for (int i = 0; i < theSets; i++)
            {
                for (int j = 0; j < theAssoc; j++)
                {
                    theIndex[i].push_back(new IndexEntry);
                }
            }
        };

        //  optional<uint64_t> find(uint64_t key){
        std::pair<uint64_t, bool> find(uint64_t key)
        {
            uint64_t set = (key >> _theBlockOffsetSize) & theSetMask;
            for (SetIter it = theIndex[set].begin();
                            it != theIndex[set].end(); it++)
            {
                IndexEntry *entry = *it;
                if (entry->key == key)
                {
                    uint64_t value = entry->value;
                    // move to MRU
                    theIndex[set].splice(theIndex[set].begin(),
                                                        theIndex[set], it);
                    assert(theIndex[set].size() == (uint64_t)theAssoc);
                    // return the pointer
                    return std::make_pair(value, true);
                }
            }
            //    return none;
            return std::make_pair(0, false);
        };

        void insert(uint64_t key, uint64_t value)
        {
            uint64_t set = (key >> _theBlockOffsetSize) & theSetMask;
            for (SetIter it = theIndex[set].begin();
                                            it != theIndex[set].end(); it++)
            {
                IndexEntry *entry = *it;
                if (entry->key == key)
                {
                    entry->value = value;
                    // move to MRU
                    theIndex[set].splice(theIndex[set].begin(),
                                                        theIndex[set], it);
                    assert(theIndex[set].size() == (uint64_t)theAssoc);
                    return;
                }
            }
            theIndex[set].push_front(new IndexEntry(key, value));
            theIndex[set].pop_back();
            assert(theIndex[set].size() == (uint64_t)theAssoc);
        };

        void saveLog(std::ostream &s)
        {
            s << theSets << "\n";
        //        for (IndexIter it = theIndex.begin(); it != theIndex.end();
        //                                it++){
        //          s << it->first << " " << it->second << "\n";
        //        }
            for (int i = 0; i < theSets; i++)
            {
                assert(theIndex[i].size() == (unsigned int)theAssoc);
                for (SetIter it = theIndex[i].begin();
                                it != theIndex[i].end(); it++)
                {
                    IndexEntry *entry = *it;
                    s << entry->key << " " << entry->value << "\n";
                }
            }
        }

        void loadState(std::istream &s)
        {
            int numSets = 0;
            uint64_t key, value;
            s >> numSets;
            assert(numSets == theSets);
            for (int i = 0; i < theSets; i++)
            {
                for (SetIter it = theIndex[i].begin();
                        it != theIndex[i].end(); it++)
                {
                    IndexEntry *entry = *it;
                    s >> key >> value;
                    entry->key = key;
                    entry->value = value;
                    // 	theIndex[i].push_back(new IndexEntry(key, value));
                }
            }
        }
    };

    struct StreamEntry
    {
        static const int maxBits = 8;

        uint64_t theRegionBase;
        bool isHead;
        bitset<maxBits> bits;

        StreamEntry()
            : theRegionBase(0), isHead(false)
        {
            bits.reset();
        };

        StreamEntry(uint64_t a, bool h)
            : theRegionBase(a), isHead(h)
        {
            bits.reset();
            bits.set(_spatialOffset);
        };
        StreamEntry(uint64_t a, bool h, uint64_t b)
            : theRegionBase(a), isHead(h), bits(b){};

        bool inRange(uint64_t anAddress, bool &prefetched)
        {
            // if (theRegionBase-(regionSize/2-spatialOffset)*theBlockSize
            // <= anAddress && anAddress < t
            //heRegionBase+(regionSize/2+spatialOffset)*theBlockSize){
            if (theRegionBase <= anAddress && anAddress <
                            (theRegionBase + _regionSize * _theBlockSize))
            {
                prefetched = bits[getIndex(anAddress)];
                return true;
            }
            else
            {
                prefetched = false;
                return false;
            }
        };

        int getIndex(uint64_t anAddress) const
        {
            // int index = (anAddress - theRegionBase)/theBlockSize
            // + (regionSize/2-spatialOffset);
            int index = (anAddress - theRegionBase) / _theBlockSize;

            assert(0 <= index && index < _regionSize);
            return index;
        };

        bool isSet(uint64_t anAddress)
        {
            int index = getIndex(anAddress);
            return bits[index];
        };
    };

    struct Stream
    {
        uint64_t theTailPos;
        list<StreamEntry> theStreamEntries;

        Stream(){};
    };

    struct Ptr
    {
        void *theStream;
        uint64_t thePos;

        Ptr(void *aStreamIdx, const uint64_t aPos)
            : theStream(aStreamIdx), thePos(aPos){};
    };

    struct Range
    {
        Ptr thePtr;
        int theLength;

        Range(void *aStream, const uint64_t aPos, int aLength)
            : thePtr(aStream, aPos), theLength(aLength){};

        Range()
            : thePtr(NULL, 0), theLength(0){};
    };

    class StreamStorage
    {
    public:
        //    virtual optional<StreamEntry> read(Range& aRange) = 0;
        virtual std::pair<StreamEntry, bool> read(Range &aRange) = 0;
        virtual bool record(StreamEntry &e) = 0;
        virtual void incrementPointer(Range &aRange) = 0;
        virtual uint64_t getPtr(uint64_t theAddress) = 0;
        virtual ~StreamStorage() {}
        virtual void saveLog(std::ostream &s) = 0;
        virtual void loadState(std::istream &s) = 0;
    };

    struct StreamTracker
    {
        int theStreamCount;
        int theTrackerSize;
        int theLookahead;

        list<Stream *> theStreams;

        StreamTracker(int sc, int ts, int la)
        {
            theStreamCount = sc;
            theTrackerSize = ts;
            theLookahead = la;

            for (int i = 0; i < theStreamCount; i++)
            {
                theStreams.push_back(new Stream);
            }
        };

        //  optional<Range> lookup(uint64_t theAddress, bool& prefetched){
        std::pair<Range, bool> lookup(uint64_t theAddress, bool &prefetched)
        {
            prefetched = false;

            for (list<Stream *>::iterator s = theStreams.begin();
                                        s != theStreams.end(); s++)
            {
                Stream *aStream = *s;
                int n = 0;
                for (list<StreamEntry>::iterator a =
                            aStream->theStreamEntries.begin();
                            a != aStream->theStreamEntries.end(); a++, n++)
                {
                    if (a->inRange(theAddress, prefetched))
                    {
                        if (!prefetched)
                            continue;
                        theStreams.splice(theStreams.begin(), theStreams, s);
                        if ((theTrackerSize - n) < theLookahead)
                        {
                            //            return optional<Range>(Range(
                            // aStream, aStream->theTailPos, theLookahead
                            // - (theTrackerSize -n)));
                            return std::make_pair((Range(aStream,
                                    aStream->theTailPos,
                                    theLookahead - (theTrackerSize - n))),
                                    true);
                        }
                        //          return optional<Range>(Range(aStream,
                        //              aStream->theTailPos, 0));
                        return std::make_pair((Range(aStream,
                                            aStream->theTailPos, 0)), true);
                    }
                }
            }
            //    return none;
            Range a;
            return std::make_pair(a, false);
        };

        bool push_back(Ptr &aPtr, const StreamEntry &entry)
        {
            Stream *aStream = (Stream *)aPtr.theStream;
            aStream->theStreamEntries.pop_front();
            aStream->theStreamEntries.push_back(entry);
            return true;
        };

        Range allocate(uint64_t aPos)
        {
            Stream *aStream = theStreams.back();
            aStream->theStreamEntries.clear();
            aStream->theTailPos = aPos;
            StreamEntry aDummyEnt;
            for (int i = 0; i < theTrackerSize; ++i)
            {
                aStream->theStreamEntries.push_back(aDummyEnt);
            }
            return Range(aStream, aPos, theLookahead);
        };
    };

    class PIFInfiniteHistory : public StreamStorage
    {
        const PIF2& _parent;
        const std::string name;
        vector<StreamEntry> history;
        map<uint64_t, uint64_t> histIndex;
        typedef map<uint64_t, uint64_t>::iterator IndexIter;

    public:
        PIFInfiniteHistory(PIF2& parent)
            : _parent(parent), name(parent.name()) {};

        //      optional<StreamEntry> read(Range& aRange){
        std::pair<StreamEntry, bool> read(Range &aRange)
        {

            if (((Stream *)aRange.thePtr.theStream)->theTailPos <
                                 history.size())
            {
                StreamEntry entry =
                    history[((Stream *)aRange.thePtr.theStream)->theTailPos];
                // cout << cycle << " Read ptr " <<
                // ((Stream*)aRange.thePtr.theStream)->theTailPos << "\n";
                return std::make_pair(entry, true);
            }
            else
            {
                // cout << cycle << " Read ptr " << "0" << " " <<
                // ((Stream*)aRange.thePtr.theStream)->theTailPos  <<  "\n";
                //          return none;
                StreamEntry entry;
                return std::make_pair(entry, false);
            }
        };
        void incrementPointer(Range &aRange)
        {
            ((Stream *)aRange.thePtr.theStream)->theTailPos++;
        };

        bool record(StreamEntry &e)
        {
            // dump();
            if (!(e.theRegionBase & ~63))
                return false;

            // history.push_back(StreamEntry(theAddress, isHead));
            history.push_back(e);
            if (e.isHead)
            {
                // histIndex[theAddress] = history.size() - 1;
                histIndex[e.theRegionBase] = history.size() - 1;
            }
            // cout << "Record " << theAddress << " "
            // << history.size() - 1 << " " << isHead << "\n";
            return true;
        };

        uint64_t getPtr(uint64_t theAddress)
        {
            if (histIndex.find(theAddress) != histIndex.end())
            {
                return histIndex[theAddress];
            }
            else
            {
                return 0;
            }
        };
        uint64_t getNext(uint64_t ptr)
        {
            return ptr + 1;
        };
        void saveLog(std::ostream &s)
        {
            s << history.size() << "\n";
            for (vector<StreamEntry>::iterator it = history.begin();
                                                it != history.end(); it++)
            {
                s << it->theRegionBase << " " << it->isHead << " "
                  << it->bits.to_ulong() << "\n";
            }
            s << histIndex.size() << "\n";
            for (IndexIter it = histIndex.begin();
                it != histIndex.end(); it++)
            {
                s << it->first << " " << it->second << "\n";
            }
        }

        void loadState(std::istream &s)
        {
            std::cerr << "History load state \n";
            int aHistorySize = 0;
            s >> aHistorySize;
            //        std::cerr << "History size "
            // << aHistorySize << std::endl;
            for (int i = 0; i < aHistorySize; i++)
            {
                uint64_t theAddress;
                bool isHead;
                unsigned long theBits;
                s >> theAddress >> isHead >> theBits;
                //          cerr << i << " "<< theAddress << " "
                // << isHead << " " << theBits << "\n";
                history.push_back(StreamEntry((theAddress & _theBlockMask),
                                                        isHead, theBits));
            }
            //        std::cerr << "History load Index state \n";
            int indexSize = 0;
            s >> indexSize;
            for (int i = 0; i < indexSize; i++)
            {
                uint64_t theAddress, thePtr;
                s >> theAddress >> thePtr;
                histIndex[(theAddress & _theBlockMask)] = thePtr;
            }
            //        std::cerr << "History load state Done \n";
        }
    };

    class PIFCircularHistoryLimitedIndex : public StreamStorage
    {
        PIF2& _parent;
        const std::string name;
        vector<StreamEntry> history;
        int historySize;
        uint64_t tail;

        Index histIndex;

        typedef map<uint64_t, uint64_t>::iterator IndexIter;

    public:
        PIFCircularHistoryLimitedIndex(PIF2& parent, int histSize,
                                        int indexSets, int indexAssoc)
            : _parent(parent), name(parent.name()),
              histIndex(indexSets, indexAssoc)
        {
            historySize = histSize;
            history.resize(historySize);
            tail = 0;
        };

        //      optional<StreamEntry> read(Range& aRange){
        std::pair<StreamEntry, bool> read(Range &aRange)
        {
            if (((Stream *)aRange.thePtr.theStream)->theTailPos
                    < history.size())
            {
                StreamEntry entry =
                    history[((Stream *)aRange.thePtr.theStream)->theTailPos];
                // return history[aRange.thePtr.thePos];
                return std::make_pair(entry, true);
            }
            else
            {
                //          return none;
                StreamEntry entry;
                return std::make_pair(entry, false);
            }
        };

        void incrementPointer(Range &aRange)
        {
            ((Stream *)aRange.thePtr.theStream)->theTailPos
                    = (((Stream *)aRange.thePtr.theStream)->theTailPos + 1)
                    % historySize;
        };

        bool record(StreamEntry &e)
        {
            if (!(e.theRegionBase & ~63))
                return false;
            // if (!(theAddress & ~63)) return false;

            // history[tail] = StreamEntry(theAddress, isHead);
            history[tail] = e;
            if (e.isHead)
            {
                // histIndex[theAddress] = tail;
                // MAX(MaxIndexSize, histIndex.size());
                histIndex.insert(e.theRegionBase, tail);
            }
            tail = (tail + 1) % historySize;
            return true;
        };

        uint64_t getPtr(uint64_t theAddress)
        {
            //        optional<uint64_t> ptr = histIndex.find(theAddress);
            std::pair<uint64_t, bool> ptr = histIndex.find(theAddress);
            if (ptr.second)
            {
                return ptr.first;
            }
            else
            {
                return 0;
            }
        };

        uint64_t getNext(uint64_t ptr)
        {
            return (ptr + 1) % historySize;
        };
        void saveLog(std::ostream &s)
        {
            s << tail << "\n";
            s << history.size() << "\n";
            for (vector<StreamEntry>::iterator it = history.begin();
                                                 it != history.end(); it++)
            {
                s << it->theRegionBase << " " << it->isHead
                  << " " << it->bits.to_ulong() << "\n";
            }
            histIndex.saveLog(s);
        };
        void loadState(std::istream &s)
        {
            std::cerr << "History load state \n";
            s >> tail;
            int aHistorySize = 0;
            s >> aHistorySize;
            assert(aHistorySize == historySize);
            for (int i = 0; i < historySize; i++)
            {
                uint64_t theAddress;
                bool isHead;
                unsigned long theBits;
                s >> theAddress >> isHead >> theBits;
                // cerr << theAddress << " "
                // << isHead << " " << theBits << "\n";
                history[i] = StreamEntry((theAddress & _theBlockMask),
                                             isHead, theBits);
            }
            histIndex.loadState(s);
        };
    };

    void resetState();

    //////////////////////////////


public:
    // void setCache(BaseCache *_cache) override;
    PacketPtr getPacket() override;

    Tick nextPrefetchReadyTime() const override
    {

        // TODO:
        // Tick pf_time = curTick() + clockPeriod() * latency;
        // return pfq.empty() ? MaxTick : curTick() + clockPeriod()* latency;
        return pfq.empty() ? MaxTick : pfq.front().readyTime;
        // if (pfq.empty())
        // if (next_ready_time < curTick()) {
        //     next_ready_time = curTick() + clockPeriod() * latency;
        // }
        // return next_ready_time;
    }

    /** @{ */
    /**
     * Ignore notifications since each sub-prefetcher already gets a
     * notification through their probes-based interface.
     */
    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override{};
    void notifyFill(const PacketPtr &pkt) override{};
    /** @} */

protected:

    #define MAX_RECENT_PFETCH 10
    std::deque<PFQEntry> recent_prefetches;
    list<PFQEntry> pfq;
    // list<uint64_t> pfq;
    StreamStorage *history;
    StreamTracker *streamTracker;
    list<StreamEntry> compactor;

private:

    Addr translateFunctional(Addr vaddr, ThreadContext* tc = nullptr);
    bool translateFunctional(RequestPtr req);
    PacketPtr createPktFromVaddr(Addr vaddr) {

        /* Create a prefetch memory request */
        RequestPtr req = createPrefetchRequest(vaddr);
        if (!translateFunctional(req)) {
            return nullptr;
        }

        // if (pfInfo.isSecure()) {
        //     req->setFlags(Request::SECURE);
        // }
        req->taskId(context_switch_task_id::Prefetcher);
        PacketPtr pkt = new Packet(req, MemCmd::HardPFReq);
        pkt->allocate();
        // if (tag_prefetch && pfInfo.hasPC()) {
        //     // Tag prefetch packet with  accessing pc
        //     pkt->req->setPC(pfInfo.getPC());
        // }
        return pkt;
    }

    PacketPtr createPkt(Addr paddr, unsigned blk_size,
                            RequestorID requestor_id,
                            bool tag_prefetch) {
        /* Create a prefetch memory request */
        RequestPtr req = std::make_shared<Request>(paddr, blk_size,
                                    Request::PREFETCH, requestor_id);

        // if (pfInfo.isSecure()) {
        //     req->setFlags(Request::SECURE);
        // }
        req->taskId(context_switch_task_id::Prefetcher);
        PacketPtr pkt = new Packet(req, MemCmd::HardPFReq);
        pkt->allocate();
        // if (tag_prefetch && pfInfo.hasPC()) {
        //     // Tag prefetch packet with  accessing pc
        //     pkt->req->setPC(pfInfo.getPC());
        // }
        return pkt;
    }
    RequestPtr
    createPrefetchRequest(Addr vaddr)
    {
        RequestPtr req = std::make_shared<Request>(
                vaddr, blkSize, 0, requestorId, vaddr, 0);
        req->setFlags(Request::PREFETCH);
        return req;
    }

    /**
     * Updates the prefetcher structures upon an instruction retired
     * @param pc PC of the instruction being retired
     */
    void notifyRetiredInst(const Addr pc);

    /**
     * Probe Listener to handle probe events from the CPU
     */
    class PrefetchListenerPC : public ProbeListenerArgBase<Addr>
    {
    public:
        PrefetchListenerPC(PIF2 &_parent, ProbeManager *pm,
                            const std::string &name)
            : ProbeListenerArgBase(pm, name),
                parent(_parent) {}
        void notify(const Addr &pc) override;

    protected:
        PIF2 &parent;
    };

    /** Array of probe listeners */
    std::vector<PrefetchListenerPC *> listenersPC;

    struct PIF2Stats : public statistics::Group
    {
        PIF2Stats(statistics::Group *parent);
        // STATS
        // Query stats
        statistics::Scalar pifQueries;
        statistics::Scalar pifQHasBeenPref;
        statistics::Scalar pifQSABHits;
        statistics::Scalar pifQIndexReset;
        statistics::Scalar pifQIndexResetMiss;
        statistics::Scalar pifQIndexResetHit;
        statistics::Scalar pfPFQHit;

        statistics::Scalar pifNRetInst;
        statistics::Scalar pifNIndexInsert;
        statistics::Scalar pifNHistWrites;
        statistics::Scalar pifNHitSpatial;
        statistics::Scalar pifNHitTemporal;
        statistics::Scalar pfIdentified;
        statistics::Scalar pfInCache;
        statistics::Scalar pfInCachePrefetched;
        statistics::Scalar pfPacketsCreated;
    } statsPIF2;

#ifdef ADDR_PRINT
    OutputStream *retInstFile = nullptr;
    OutputStream *predInstFile = nullptr;
    OutputStream *histFile = nullptr;
    OutputStream *sabFile = nullptr;
#endif

public:
    PIF2(const PIF2PrefetcherParams &p);
    ~PIF2() = default;

    // void calculatePrefetch(const PrefetchInfo &pfi,
    //                         std::vector<AddrPriority> &addresses);

    /**
     * Add a SimObject and a probe name to monitor the retired instructions
     * @param obj The SimObject pointer to listen from
     * @param name The probe name
     */
    void addEventProbeRetiredInsts(SimObject *obj, const char *name);

    void memInvalidate() override
    {
        resetState();
    }
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_PIF2_HH__
