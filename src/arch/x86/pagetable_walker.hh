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
 */

#ifndef __ARCH_X86_PAGE_TABLE_WALKER_HH__
#define __ARCH_X86_PAGE_TABLE_WALKER_HH__

#include <vector>

#include "arch/generic/mmu.hh"
#include "arch/x86/pagetable.hh"
#include "arch/x86/tlb.hh"
#include "arch/x86/tlbl2.hh"
#include "base/types.hh"
#include "mem/packet.hh"
#include "mem/page_table.hh"
#include "params/X86PagetableWalker.hh"
#include "sim/clocked_object.hh"
#include "sim/faults.hh"
#include "sim/system.hh"

namespace gem5
{

class ThreadContext;

namespace X86ISA
{
    class Walker : public ClockedObject
    {
      protected:
        // Port for accessing memory
        class WalkerPort : public RequestPort
        {
          public:
            WalkerPort(const std::string &_name, Walker * _walker) :
                  RequestPort(_name, _walker), walker(_walker)
            {}

          protected:
            Walker *walker;

            bool recvTimingResp(PacketPtr pkt);
            void recvReqRetry();
        };

        friend class WalkerPort;
        WalkerPort port;

        // State to track each walk of the page table
        class WalkerState
        {
          friend class Walker;
          private:
            enum State
            {
                Ready,
                Waiting,
                // Long mode
                LongPML4, LongPDP, LongPD, LongPTE,
                // PAE legacy mode
                PAEPDP, PAEPD, PAEPTE,
                // Non PAE legacy mode with and without PSE
                PSEPD, PD, PTE
            };

          protected:
            Walker *walker;
            ThreadContext *tc;
            RequestPtr req;
            State state;
            State nextState;
            int dataSize;
            bool enableNX;
            unsigned inflight;
            TlbEntry entry;
            PacketPtr read;
            std::vector<PacketPtr> writes;
            Fault timingFault;
            BaseMMU::Translation * translation;
            BaseMMU::Mode mode;
            bool functional;
            bool timing;
            bool retrying;
            bool started;
            bool squashed;
            bool fixed_lat;
            Tick walk_lat;
            TLB::TLBWalkerAction action;
          public:
            WalkerState(Walker * _walker, BaseMMU::Translation *_translation,
                        const RequestPtr &_req, bool _isFunctional = false) :
                walker(_walker), req(_req), state(Ready),
                nextState(Ready), inflight(0),
                translation(_translation),
                functional(_isFunctional), timing(false),
                retrying(false), started(false), squashed(false),
                fixed_lat(_walker->fixed_lat), walk_lat(_walker->walk_lat),
                action(_walker->action)
            {
            }
            void initState(ThreadContext * _tc, BaseMMU::Mode _mode,
                           bool _isTiming = false);
            Fault startWalk();
            Fault startFunctional(Addr &addr, unsigned &logBytes);
            bool recvPacket(PacketPtr pkt);
            unsigned numInflight() const;
            bool isRetrying();
            bool wasStarted();
            bool isTiming();
            void retry();
            void squash();
            std::string name() const {return walker->name();}

            TLB::TLBWalkerAction getAction() { return action; }
            Tick getLatency() { return walk_lat; }

          private:
            void setupWalk(Addr vaddr);
            Fault stepWalk(PacketPtr &write);
            void sendPackets();
            void endWalk();
            Fault pageFault(bool present);
        };

        friend class WalkerState;
        // State for timing and atomic accesses (need multiple per walker in
        // the case of multiple outstanding requests in timing mode)
        std::list<WalkerState *> currStates;
        // State for functional accesses (only need one of these per walker)
        WalkerState funcState;

        struct WalkerSenderState : public Packet::SenderState
        {
            WalkerState * senderWalk;
            WalkerSenderState(WalkerState * _senderWalk) :
                senderWalk(_senderWalk) {}
        };

      public:
        // Kick off the state machine.
        Fault start(ThreadContext * _tc, BaseMMU::Translation *translation,
                const RequestPtr &req, BaseMMU::Mode mode);
        Fault startFunctional(ThreadContext * _tc, Addr &addr,
                unsigned &logBytes, BaseMMU::Mode mode);
        Port &getPort(const std::string &if_name,
                      PortID idx=InvalidPortID) override;

      protected:
        // The TLB we're supposed to load.
        TLB * tlb;
        System * sys;
        RequestorID requestorId;

        // Variables to ddo fixed/latency emulated table walks
        bool fixed_lat;
        Tick walk_lat;
        TLB::TLBWalkerAction action;

        // The number of outstanding walks that can be squashed per cycle.
        unsigned numSquashable;

        /** Delay cycles */
        const Cycles delay;

        // Wrapper for checking for squashes before starting a translation.
        void startWalkWrapper();
        void finishedFixedLatWalk();
        int coalesceWalkRequests(Addr vpn, bool isLarge,
                const EmulationPageTable::Entry *pte);

        /**
         * Event used to call startWalkWrapper.
         **/
        EventFunctionWrapper startWalkWrapperEvent;
        EventFunctionWrapper fixedLatEvent;

        // Functions for dealing with packets.
        bool recvTimingResp(PacketPtr pkt);
        void recvReqRetry();
        bool sendTiming(WalkerState * sendingState, PacketPtr pkt,unsigned n);

      public:

        void setTLB(TLB * _tlb)
        {
            tlb = _tlb;
        }

        void setLatAndAction(Tick lat, TLB::TLBWalkerAction action);

        typedef X86PagetableWalkerParams Params;

        Walker(const Params &params) :
            ClockedObject(params), port(name() + ".port", this),
            funcState(this, NULL, NULL, true), tlb(NULL), sys(params.system),
            requestorId(sys->getRequestorId(this)),
            fixed_lat(false), walk_lat(0UL),
            numSquashable(params.num_squash_per_cycle),
            delay(params.delay),
            startWalkWrapperEvent([this]{ startWalkWrapper(); }, name()),
            fixedLatEvent([this]{ finishedFixedLatWalk(); }, name())
        {
        }
    };

} // namespace X86ISA
} // namespace gem5

#endif // __ARCH_X86_PAGE_TABLE_WALKER_HH__
