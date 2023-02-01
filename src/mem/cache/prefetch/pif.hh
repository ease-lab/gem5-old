/**
 * Copyright (c) 2019 Metempsy Technology Consulting
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

/** Implementation of the 'Proactive Instruction Fetch' prefetcher
 *  Reference:
 *    Ferdman, M., Kaynak, C., & Falsafi, B. (2011, December).
 *    Proactive instruction fetch.
 *    In Proceedings of the 44th Annual IEEE/ACM International Symposium
 *    on Microarchitecture (pp. 152-162). ACM.
 */

#ifndef __MEM_CACHE_PREFETCH_PIF_HH__
#define __MEM_CACHE_PREFETCH_PIF_HH__

// #define ADDR_PRINT

#include <deque>
#include <vector>
#include <list>
#include <bitset>


#include "base/circular_queue.hh"
#include "base/output.hh"
#include "cpu/base.hh"
#include "mem/cache/prefetch/associative_set.hh"
#include "mem/cache/prefetch/queued.hh"

#define _max_bits 8
#define _regionSize 8
#define _blk_size 64


namespace gem5
{

struct PIFPrefetcherParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

class PIF : public Queued
{
    private:
        /** Number of preceding and subsequent spatial addresses to compact */
        const unsigned int precSize;
        const unsigned int succSize;
        /** Number of entries used for the temporal compactor */
        const unsigned int maxCompactorEntries;
        /** Use Block address instead of full instruction address to create
         *  records*/
        const bool useBlkAddr;
        /** Make prediction based on the retired PC instead of the miss
         * addresses  */
        const bool predictOnRetiredPC;

        /** Stream address buffer size */
        const unsigned sabSize;
        /** The look ahead distance defines the threshold when new entries
         * filled in. If the matching index in the SAB will exceed this
         * threashold new entries will be filled by replacing the first
         * one in the buffer.
         */
        const unsigned lookahead;

        const bool compactor_lru;


        /**
         * The compactor tracks retired instructions addresses, leveraging the
         * spatial and temporal locality among instructions for compaction. It
         *comprises the spatial and temporal compaction mechanisms.
         *
         * Taking advantage of the spatial locality across instruction blocks,
         * the spatial compactor combines instruction-block addresses that fall
         * within a 'spatial region', a group of adjacent instruction blocks.
         * When an instruction outside the current spatial region retires, the
         * existing spatial region is sent to the temporal compactor.
         *
         * The temporal compactor tracks a small number of the
         * most-recently-observed spatial region records.
         */
        struct CompactorEntry
        {
            Addr trigger;
            std::vector<bool> prec;
            std::vector<bool> succ;
            std::bitset<_max_bits> bits;

            CompactorEntry() { trigger = MaxAddr; }
            CompactorEntry(Addr, unsigned int, unsigned int);

            /**
             * Checks if a given address is in the same defined spatial region
             * as the compactor entry.
             * @param addr Address to check if it's inside the spatial region
             * @param log_blk_distance log_2(block size of the cache)
             * @param update if true, set the corresponding succ/prec entry
             * @return TRUE if they are in the same spatial region, FALSE
             *   otherwise
             */
            bool inSameSpatialRegion(Addr addr, unsigned int log_blk_size,
                                     bool update);
            /**
             * Checks if the provided address is contained in this spatial
             * region and if its corresponding bit vector entry is set
             * @param target address to check
             * @param log_blk_distance log_2(block size of the cache)
             * @return TRUE if target has its bit set
             */
            bool hasAddress(Addr target, unsigned int log_blk_size) const;


            /**
             * Checks if the provided compact entry is a subset of
             * of this
             * @param other Other compactor entry.
             * @return TRUE To be true the trigger address must be the same
             *         as well as the bitvector of the other need to be a
             *         subset of this one.
             */
            bool isSupersetOf(CompactorEntry& other) const;

            /**
             * Number of blocks recorded in this entry.
            */
            unsigned numBlocks() const;

            int getIndex(uint64_t addr, unsigned int log_blk_size) const;

            std::string
            print()
            {
              std::ostringstream oss;
              oss << "x" << std::hex << trigger << " [";
              // oss << numBlocks();
              oss << bits.to_string();
              // for (auto b : prec) oss << (b) ? "1" : "0";
              // oss << "|";
              // for (auto b : succ) oss << (b) ? "1" : "0";
              oss << "]";
              return oss.str();
            }

            /**
             * Fills the provided vector with the predicted addresses using the
             * recorded bit vectors of the entry
             * @param log_blk_distance log_2(block size of the cache)
             * @param addresses reference to a vector to add the generated
             * addresses
             */
            void getPredictedAddresses(unsigned int log_blk_size,
                    std::list<Addr> &addresses) const;
          private:
            /**
             * Computes the distance, in cache blocks, from an address to the
             * trigger of the entry.
             * @param addr address to compute the distance from the trigger
             * @param log_blk_distance log_2(block size of the cache)
             * @result distance in cache blocks from the address to the trigger
             */
            Addr distanceFromTrigger(Addr addr,
                                     unsigned int log_blk_size) const;
        };

        CompactorEntry spatialCompactor;
        std::list<CompactorEntry> temporalCompactor;

        /**
         * History buffer is a circular buffer that stores the sequence of
         * retired instructions in FIFO order.
         */
        using HistoryBuffer = CircularQueue<CompactorEntry>;
        HistoryBuffer historyBuffer;
        typedef HistoryBuffer::iterator HistoryPtr;

        struct IndexEntry : public TaggedEntry
        {
            HistoryPtr historyIt;
        };
        /**
         * The index table is a small cache-like structure that facilitates
         * fast search of the history buffer.
         */
        AssociativeSet<IndexEntry> index;

        /**
         * A Stream Address Buffer (SAB) tracks a window of consecutive
         * spatial regions. The SAB mantains a pointer to the sequence in the
         * history buffer, initiallly set to the pointer taken from the index
         * table
         */
        std::list<HistoryPtr> streamAddressBuffer;

        /**
         * A list of prefetch candidates that will be generated from the
         * prediction function.
         */
        std::list<Addr> prefetchCandiates;

        /**
         * Prediction functionality.
         * Looks up the given PC in the SAB to create new prefetch
         * canditates. Predictions will be added to the candidates list.
         * @param pc PC used to make prediction.
         */
        void predict(const Addr pc);

        /**
         * Recording functionality.
         * Adds the current PC to the current active temporal
         * stream. Use spatial, temporal compactor to compress meta data.
         * @param pc PC of the instruction being retired
        */
        void record(const Addr pc);


        /**
         * Updates the prefetcher structures upon an instruction retired
         * @param pc PC of the instruction being retired
         */
        void notifyRetiredInst(const Addr pc);

        std::vector<ProbeListener *> listeners;
        BaseCPU *cpu;


        struct PIFStats : public statistics::Group
        {
            PIFStats(statistics::Group *parent);
            // STATS
            // Query stats
            statistics::Scalar pifQueries;
            statistics::Scalar pifQHasBeenPref;
            statistics::Scalar pifQSABHits;
            statistics::Scalar pifQIndexReset;
            statistics::Scalar pifQIndexResetMiss;
            statistics::Scalar pifQIndexResetHit;
            statistics::Scalar translationFail;
            statistics::Scalar translationSuccess;


            statistics::Scalar pifNRetInst;
            statistics::Scalar pifRecordPC;
            statistics::Scalar pifNIndexInsert;
            statistics::Scalar pifNIndexOverride;
            statistics::Scalar pifNHistWrites;
            statistics::Scalar pifNHitSpatial;
            statistics::Scalar pifNHitTemporal;

        } statsPIF;


    public:
        PIF(const PIFPrefetcherParams &p);
        ~PIF() = default;

        void calculatePrefetch(const PrefetchInfo &pfi,
                               std::vector<AddrPriority> &addresses);

        void regProbeListeners() override;

        /**
         * Invalidates all entries of the history and the index
        */
        void memInvalidate() override
        {
          index.invalidateAll();
          streamAddressBuffer.clear();

          historyBuffer.flush();
          temporalCompactor.clear();
        }
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_PIF_HH__
