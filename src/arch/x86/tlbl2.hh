/*
 * Copyright (c) 2007 The Hewlett-Packard Development Company
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
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
 *
 * Authors: Gabe Black
 */

#ifndef __ARCH_X86_TLBL2_HH__
#define __ARCH_X86_TLBL2_HH__

#include <list>
#include <vector>

#include "arch/generic/tlb.hh"
#include "arch/x86/pagetable.hh"
#include "arch/x86/tlb.hh"
#include "base/trie.hh"
#include "mem/request.hh"
#include "params/X86TLBL2.hh"

class ThreadContext;

namespace X86ISA
{
    class Walker;

    class TLBL2 : public TLB
    {
      protected:
        friend class Walker;

        typedef std::list<TlbEntry *> EntryList;

        uint32_t configAddress;

      public:

        typedef X86TLBL2Params Params;
        TLBL2(const Params *p);

        TlbEntry *lookup(Addr va, bool update_lru = true);

        void setConfigAddress(uint32_t addr);

      protected:

        Walker * walker;

      public:
        Walker *getWalker();

        void flushAll();

        void flushNonGlobal();

        void demapPage(Addr va, uint64_t asn);

      protected:
        //L1 TLBL2
        uint32_t size_l1_4k;
        uint32_t size_l1_2m;
        uint32_t size_l2;

        uint32_t assoc_l1_4k;
        uint32_t assoc_l1_2m;
        uint32_t assoc_l2;

        uint32_t set_l1_4k;
        uint32_t set_l1_2m;
        uint32_t set_l2;

        uint32_t set_bits_l1_4k;
        uint32_t set_bits_l1_2m;
        uint32_t set_bits_l2;

        Tick walk_lat;

        std::vector<TlbEntry> tlb_l1_4k;
        std::vector<TlbEntry> tlb_l1_2m;
        std::vector<TlbEntry> tlb_l2;

        EntryList freeList;

        TlbEntryTrie trie;
        uint64_t lruSeq;

        // Statistics
        Stats::Scalar rdAccesses;
        Stats::Scalar wrAccesses;
        Stats::Scalar rdMisses;
        Stats::Scalar wrMisses;

        Stats::Scalar walkCycles;
        Stats::Scalar walks;
        Stats::Scalar squashedWalks;

        Fault translateInt(const RequestPtr &req, ThreadContext *tc);

        Fault translate(const RequestPtr &req, ThreadContext *tc,
                BaseTLB::Translation *translation, BaseTLB::Mode mode,
                bool &delayedResponse, bool timing);

        // Inflight translation context
        bool transInflight;
        ThreadContext *inflight_tc;
        RequestPtr inflight_req;
        Mode inflight_mode;
        Translation *inflight_trans;
        EventFunctionWrapper walkCompleteEvent;


      public:
        void inc_walk_cycles(Tick cycles)
        {
            walkCycles += cycles;
        }

        void inc_walks()
        {
            walks++;
        }

        void inc_squashed_walks(unsigned num_squashed)
        {
            squashedWalks+=num_squashed;
        }

        void completeTranslation();

        void evictLRU();

        uint64_t
        nextSeq()
        {
            return ++lruSeq;
        }

        Fault translateAtomic(
            const RequestPtr &req, ThreadContext *tc, BaseTLB::Mode mode);
        void translateTiming(
            const RequestPtr &req, ThreadContext *tc,
            BaseTLB::Translation *translation, BaseTLB::Mode mode);

        /**
         * Do post-translation physical address finalization.
         *
         * Some addresses, for example requests going to the APIC,
         * need post-translation updates. Such physical addresses are
         * remapped into a "magic" part of the physical address space
         * by this method.
         *
         * @param req Request to updated in-place.
         * @param tc Thread context that created the request.
         * @param mode Request type (read/write/execute).
         * @return A fault on failure, NoFault otherwise.
         */
        Fault finalizePhysical(const RequestPtr &req, ThreadContext *tc,
                               BaseTLB::Mode mode) const;

        TlbEntry *insert(Addr vpn, const TlbEntry &entry);

        /*
         * Function to register Stats
         */
        void regStats();

        // Checkpointing
        void serialize(CheckpointOut &cp) const;
        void unserialize(CheckpointIn &cp);

        /**
         * Get the table walker port. This is used for
         * migrating port connections during a CPU takeOverFrom()
         * call. For architectures that do not have a table walker,
         * NULL is returned, hence the use of a pointer rather than a
         * reference. For X86 this method will always return a valid
         * port pointer.
         *
         * @return A pointer to the walker port
         */
        Port *getTableWalkerPort();
    };
}

#endif // __ARCH_X86_TLBL2_HH__
