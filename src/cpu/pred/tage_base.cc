/*
 * Copyright (c) 2014 The University of Wisconsin
 *
 * Copyright (c) 2006 INRIA (Institut National de Recherche en
 * Informatique et en Automatique  / French National Research Institute
 * for Computer Science and Applied Mathematics)
 *
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

/* @file
 * Implementation of a TAGE branch predictor
 */

#include "cpu/pred/tage_base.hh"

#include <regex>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/output.hh"
#include "base/random.hh"
#include "debug/Fetch.hh"
#include "debug/Tage.hh"

namespace gem5
{

namespace branch_prediction
{

TAGEBase::TAGEBase(const TAGEBaseParams &p)
   : SimObject(p),
     logRatioBiModalHystEntries(p.logRatioBiModalHystEntries),
     nHistoryTables(p.nHistoryTables),
     tagTableCounterBits(p.tagTableCounterBits),
     tagTableUBits(p.tagTableUBits),
     histBufferSize(p.histBufferSize),
     minHist(p.minHist),
     maxHist(p.maxHist),
     pathHistBits(p.pathHistBits),
     tagTableTagWidths(p.tagTableTagWidths),
     logTagTableSizes(p.logTagTableSizes),
     threadHistory(p.numThreads),
     logUResetPeriod(p.logUResetPeriod),
     initialTCounterValue(p.initialTCounterValue),
     numUseAltOnNa(p.numUseAltOnNa),
     useAltOnNaBits(p.useAltOnNaBits),
     maxNumAlloc(p.maxNumAlloc),
     calcIdxUsePC(p.calcIdxUsePC),
     calcIdxUsePath(p.calcIdxUsePath),
     calcTagUseHist(p.calcTagUseHist),
     takenOnlyHistory(p.takenOnlyHistory),
     calcHistUseTarget(p.calcHistUseTarget),
     allowLatePredict(p.allowLatePredict),
     noDummyAllocate(p.noDummyAllocate),
     noSkip(p.noSkip),
     speculativeHistUpdate(p.speculativeHistUpdate),
     instShiftAmt(p.instShiftAmt),
     initialized(false),
     stats(this, nHistoryTables)
{
    if (noSkip.empty()) {
        // Set all the table to enabled by default
        noSkip.resize(nHistoryTables + 1, true);
    }
}

TAGEBase::BranchInfo*
TAGEBase::makeBranchInfo() {
    return new BranchInfo(*this);
}

void
TAGEBase::init()
{
    if (initialized) {
       return;
    }

    // Current method for periodically resetting the u counter bits only
    // works for 1 or 2 bits
    // Also make sure that it is not 0
    assert(tagTableUBits <= 2 && (tagTableUBits > 0));

    // we use int type for the path history, so it cannot be more than
    // its size
    assert(pathHistBits <= (sizeof(int)*8));

    // initialize the counter to half of the period
    assert(logUResetPeriod != 0);
    tCounter = initialTCounterValue;

    assert(histBufferSize > maxHist * 2);

    useAltPredForNewlyAllocated.resize(numUseAltOnNa, 0);

    for (auto& history : threadHistory) {
        history.pathHist = 0;
        history.globalHistory = new uint8_t[histBufferSize];
        history.gHist = history.globalHistory;
        memset(history.gHist, 0, histBufferSize);
        history.ptGhist = 0;
        history.commitPtGhist = 0;
    }

    histLengths = new int [nHistoryTables+1];

    calculateParameters();

    assert(tagTableTagWidths.size() == (nHistoryTables+1));
    assert(logTagTableSizes.size() == (nHistoryTables+1));

    // First entry is for the Bimodal table and it is untagged in this
    // implementation
    assert(tagTableTagWidths[0] == 0);

    for (auto& history : threadHistory) {
        history.computeIndices = new FoldedHistory[nHistoryTables+1];
        history.computeTags[0] = new FoldedHistory[nHistoryTables+1];
        history.computeTags[1] = new FoldedHistory[nHistoryTables+1];

        history.commitComputeIndices = new FoldedHistory[nHistoryTables+1];
        history.commitComputeTags[0] = new FoldedHistory[nHistoryTables+1];
        history.commitComputeTags[1] = new FoldedHistory[nHistoryTables+1];

        initFoldedHistories(history);
    }

    const uint64_t bimodalTableSize = 1ULL << logTagTableSizes[0];
    btablePrediction.resize(bimodalTableSize, false);
    btableHysteresis.resize(bimodalTableSize >> logRatioBiModalHystEntries,
                            true);

    gtable = new TageEntry*[nHistoryTables + 1];
    buildTageTables();

    tableIndices = new int [nHistoryTables+1];
    tableTags = new int [nHistoryTables+1];
    initialized = true;
    file_cnt = 0;
}

void
TAGEBase::reset(uint start, uint end, uint val)
{
    // std::string fn = "tage_dump_" + std::to_string(file_cnt) + ".csv";
    // dump(fn);
    DPRINTF(Tage, "Reset TAGE tables %i->%i\n", start, end);


    if (start == 0 && end > 0) {
        // Reset the base predictor
        unsigned cnt = 0;
        bool _val = val ? true : false;
        for (int i = 0; i < btablePrediction.size(); i++) {
            // val = random_mt.random<int>() & 1;
            cnt = (btablePrediction[i]) ? cnt+1 : cnt;
            btablePrediction[i] = _val;
        }
        warn("Reset BIM direction to %i. "
                    "Was: %i of %i (%.2f) where set\n",
                        _val, cnt, btablePrediction.size(),
                        (float)cnt / (float)btablePrediction.size());

        _val = !_val;
        cnt = 0;
        for (int i = 0; i < btableHysteresis.size(); i++) {
            // val = random_mt.random<int>() & 1;
            cnt = (btableHysteresis[i]) ? cnt+1 : cnt;
            btableHysteresis[i] = _val;
        }
        warn("Reset BIM hysteresis to %i. "
                    "Was: %i of %i (%.2f) where set.\n",
                        _val, cnt, btableHysteresis.size(),
                        (float)cnt / (float)btableHysteresis.size());
    }

    // TAGE tables
    int i = (start == 0) ? 1 : start;
    for (; i <= nHistoryTables && i < end; i++) {
        unsigned cnt = 0;
        unsigned ctr_u = 0;
        for (int j = 0; j < (1<<(logTagTableSizes[i])); j++) {
            // if (gtable[i][j].correct_pred > 1) {
                gtable[i][j].tag = 0;
                gtable[i][j].u = 0;
            // }
            // gtable[i][j].tag = 0;
            cnt = (gtable[i][j].correct_pred) ? cnt+1 : cnt;
            // gtable[i][j].u = 0;
            ctr_u = (gtable[i][j].ctr != 0) ? ctr_u+1 : ctr_u;
        }
        warn("Reset TAGE Table %i. Was: %i of %i (%.2f) useful. %i (%.2f) "
                "used.\n",
                i, cnt, (1<<(logTagTableSizes[i])),
                (float)cnt / (float)(1<<(logTagTableSizes[i])),
                ctr_u,
                (float)ctr_u / (float)(1<<(logTagTableSizes[i])));
    }

    // if (file_cnt > 2) {
        // fn = "tage_restore_" + std::to_string(file_cnt) + ".csv";
        // restore(fn);
    // }
    // file_cnt += 1;
}


void
TAGEBase::dump(const std::string& filename)
{
    DPRINTF(Tage, "Dump TAGE tables to %s\n", filename);

    std::ofstream fileStream(simout.resolve(filename), std::ios::out);
    if (!fileStream.good())
        panic("Could not open %s for writing\n", filename);



    ccprintf(fileStream,"TTBank,TTIdx,ctr,u,tag,correct,pc\n");


    // TAGE tables
    int i = 1;
    for (; i <= nHistoryTables; i++) {
        for (int j = 0; j < (1<<(logTagTableSizes[i])); j++) {

            if (gtable[i][j].tag != 0) {
                ccprintf(fileStream,"%d,%d,%i,%i,%d,%d,%llu\n",
                            i,j,
                            gtable[i][j].ctr, gtable[i][j].u,
                            gtable[i][j].tag, gtable[i][j].correct_pred,
                            gtable[i][j].pc);
            }
        }
    }
    fileStream.close();
}


void
TAGEBase::restore(const std::string& filename)
{
    DPRINTF(Tage, "Restore TAGE tables to %s\n", filename);

    std::ifstream fileStream(simout.resolve(filename), std::ios::in);
    // if (!fileStream.good())
    //     panic("Could not open %s for writing\n", filename);


    if (fileStream.is_open()){   //checking whether the file is open

        std:: string line;
        std::regex rgx("(\\d+),(\\d+),(-?\\d+),(\\d+),(\\d+),\\d+,(\\d+)");
        std::smatch matches;

        while (std::getline(fileStream, line))
        {
            // std::cout << line << std::endl;

            if (std::regex_search(line, matches, rgx)) {

                auto bank = std::stoi(matches[1]);
                auto idx = std::stoi(matches[2]);
                auto ctr = std::stoi(matches[3]);
                auto tag = std::stoi(matches[5]);
                auto pc = std::stoull(matches[6]);

                assert(bank <= nHistoryTables);
                assert(idx < (1<<(logTagTableSizes[bank])));

                gtable[bank][idx].ctr = ctr;
                gtable[bank][idx].tag = tag;
                gtable[bank][idx].pc = pc;
            }
        }
        fileStream.close(); //close the file object.
    } else {
        warn("Could not open %s for restoring\n", filename);
    }
}







void
TAGEBase::initFoldedHistories(ThreadHistory & history)
{
    for (int i = 1; i <= nHistoryTables; i++) {
        history.computeIndices[i].init(
            histLengths[i], (logTagTableSizes[i]));
        history.computeTags[0][i].init(
            history.computeIndices[i].origLength, tagTableTagWidths[i]);
        history.computeTags[1][i].init(
            history.computeIndices[i].origLength, tagTableTagWidths[i]-1);

        history.commitComputeIndices[i].init(
            histLengths[i], (logTagTableSizes[i]));
        history.commitComputeTags[0][i].init(
            history.commitComputeIndices[i].origLength, tagTableTagWidths[i]);
        history.commitComputeTags[1][i].init(
            history.commitComputeIndices[i].origLength, tagTableTagWidths[i]-1);

        DPRINTF(Tage, "HistLength:%d, TTSize:%d, TTTWidth:%d\n",
                histLengths[i], logTagTableSizes[i], tagTableTagWidths[i]);
        warn("HistLength:%d, TTSize:%d, TTTWidth:%d\n",
                histLengths[i], logTagTableSizes[i], tagTableTagWidths[i]);
    }
}

void
TAGEBase::buildTageTables()
{
    for (int i = 1; i <= nHistoryTables; i++) {
        gtable[i] = new TageEntry[1<<(logTagTableSizes[i])];
    }
}

void
TAGEBase::calculateParameters()
{
    histLengths[1] = minHist;
    histLengths[nHistoryTables] = maxHist;

    for (int i = 2; i <= nHistoryTables; i++) {
        histLengths[i] = (int) (((double) minHist *
                       pow ((double) (maxHist) / (double) minHist,
                           (double) (i - 1) / (double) ((nHistoryTables- 1))))
                       + 0.5);
    }
}



int
TAGEBase::bindex(Addr pc_in) const
{
    return ((pc_in >> instShiftAmt) & ((1ULL << (logTagTableSizes[0])) - 1));
}

int
TAGEBase::F(int A, int size, int bank) const
{
    int A1, A2;

    A = A & ((1ULL << size) - 1);
    A1 = (A & ((1ULL << logTagTableSizes[bank]) - 1));
    A2 = (A >> logTagTableSizes[bank]);
    A2 = ((A2 << bank) & ((1ULL << logTagTableSizes[bank]) - 1))
       + (A2 >> (logTagTableSizes[bank] - bank));
    A = A1 ^ A2;
    A = ((A << bank) & ((1ULL << logTagTableSizes[bank]) - 1))
      + (A >> (logTagTableSizes[bank] - bank));
    return (A);
}

// gindex computes a full hash of pc, ghist and pathHist
int
TAGEBase::gindex(ThreadID tid, Addr pc, int bank, bool spec) const
{
    int index = 0;
    int hlen = (histLengths[bank] > pathHistBits) ? pathHistBits :
                                                    histLengths[bank];
    const unsigned int shiftedPc = pc >> instShiftAmt;

    // Calculate the table index from the global history (folded)
    if (spec) {
        index = threadHistory[tid].computeIndices[bank].comp;
    } else {
        index = threadHistory[tid].commitComputeIndices[bank].comp;
    }

    // PC
    if (calcIdxUsePC) {
        index = index ^ shiftedPc ^
                (shiftedPc >> ((int) abs(logTagTableSizes[bank] - bank) + 1));
    }

    // Path
    if (calcIdxUsePath) {
        index = index ^ F(threadHistory[tid].pathHist, hlen, bank);
    }

    return (index & ((1ULL << (logTagTableSizes[bank])) - 1));
}


// Tag computation
uint16_t
TAGEBase::gtag(ThreadID tid, Addr pc, int bank, bool spec) const
{
    // Calculate TAG from PC and history.
    int tag = (pc >> instShiftAmt);
    if (calcTagUseHist) {
        if (spec) {
            tag = tag ^ threadHistory[tid].computeTags[0][bank].comp ^
                      (threadHistory[tid].computeTags[1][bank].comp << 1);
        } else {
            tag = tag ^ threadHistory[tid].commitComputeTags[0][bank].comp ^
                       (threadHistory[tid].commitComputeTags[1][bank].comp << 1);
        }
    }
    return (tag & ((1ULL << tagTableTagWidths[bank]) - 1));
}


// Up-down saturating counter
template<typename T>
void
TAGEBase::ctrUpdate(T & ctr, bool taken, int nbits)
{
    assert(nbits <= sizeof(T) << 3);
    if (taken) {
        if (ctr < ((1 << (nbits - 1)) - 1))
            ctr++;
    } else {
        if (ctr > -(1 << (nbits - 1)))
            ctr--;
    }
}

// int8_t and int versions of this function may be needed
template void TAGEBase::ctrUpdate(int8_t & ctr, bool taken, int nbits);
template void TAGEBase::ctrUpdate(int & ctr, bool taken, int nbits);

// Up-down unsigned saturating counter
void
TAGEBase::unsignedCtrUpdate(uint8_t & ctr, bool up, unsigned nbits)
{
    assert(nbits <= sizeof(uint8_t) << 3);
    if (up) {
        if (ctr < ((1 << nbits) - 1))
            ctr++;
    } else {
        if (ctr)
            ctr--;
    }
}

// Bimodal prediction
bool
TAGEBase::getBimodePred(Addr pc, BranchInfo* bi) const
{
    return btablePrediction[bi->bimodalIndex];
}


// Update the bimodal predictor: a hysteresis bit is shared among N prediction
// bits (N = 2 ^ logRatioBiModalHystEntries)
void
TAGEBase::baseUpdate(Addr pc, bool taken, BranchInfo* bi)
{
    int inter = (btablePrediction[bi->bimodalIndex] << 1)
        + btableHysteresis[bi->bimodalIndex >> logRatioBiModalHystEntries];
    if (taken) {
        if (inter < 3)
            inter++;
    } else if (inter > 0) {
        inter--;
    }
    const bool pred = inter >> 1;
    const bool hyst = inter & 1;
    btablePrediction[bi->bimodalIndex] = pred;
    btableHysteresis[bi->bimodalIndex >> logRatioBiModalHystEntries] = hyst;
    DPRINTF(Tage, "Updating branch %lx, pred:%d, hyst:%d\n", pc, pred, hyst);
}

// shifting the global history:  we manage the history in a big table in order
// to reduce simulation time
void
TAGEBase::updateGHist(uint8_t * &h, bool dir, uint8_t * tab, int &pt)
{
    if (pt == 0) {
        DPRINTF(Tage, "Rolling over the histories\n");
         // Copy beginning of globalHistoryBuffer to end, such that
         // the last maxHist outcomes are still reachable
         // through pt[0 .. maxHist - 1].
         for (int i = 0; i < maxHist; i++)
             tab[histBufferSize - maxHist + i] = tab[i];
         pt =  histBufferSize - maxHist;
         h = &tab[pt];
         threadHistory[0].commitPtGhist += histBufferSize - maxHist;
    }
    pt--;
    h--;
    h[0] = (dir) ? 1 : 0;
}

void
TAGEBase::updateGHist(uint8_t * &h, bool dir, Addr pc, Addr target,
                      uint8_t * tab, int &pt)
{
    // For not-taken branches we can skip the global history update.
    if (!dir) return;

    if (pt < 2 ) {
        DPRINTF(Tage, "Rolling over the histories\n");
         // Copy beginning of globalHistoryBuffer to end, such that
         // the last maxHist outcomes are still reachable
         // through pt[0 .. maxHist - 1].
         for (int i = 0; i < maxHist; i++)
             tab[histBufferSize - maxHist + i] = tab[i];
         pt =  histBufferSize - maxHist;

         h = &tab[pt];
         threadHistory[0].commitPtGhist += histBufferSize - maxHist;
    }

    // Use target and pc to build a 2bit target history
    uint64_t targethash = (pc >> 2) ^ (target >> 3);

    // uint64_t targethash = (pc >> (instShiftAmt + 2)) ^
    //                       (target >> (instShiftAmt + 3));

    pt -= 2;
    h -= 2;
    h[0] = (targethash) & 1;
    h[1] = (targethash >> 1) & 1;
}

void
TAGEBase::calculateIndicesAndTags(ThreadID tid, Addr branch_pc,
                                  BranchInfo* bi, bool spec)
{
    // computes the table addresses and the partial tags
    for (int i = 1; i <= nHistoryTables; i++) {
        tableIndices[i] = gindex(tid, branch_pc, i, spec);
        bi->tableIndices[i] = tableIndices[i];
        tableTags[i] = gtag(tid, branch_pc, i, spec);
        bi->tableTags[i] = tableTags[i];
    }
}

unsigned
TAGEBase::getUseAltIdx(BranchInfo* bi, Addr branch_pc)
{
    // There is only 1 counter on the base TAGE implementation
    return 0;
}

bool
TAGEBase::tagePredict(ThreadID tid, Addr branch_pc,
              bool cond_branch, BranchInfo* bi)
{
    Addr pc = branch_pc;
    bool pred_taken = true;

    if (cond_branch) {
        // TAGE prediction

        calculateIndicesAndTags(tid, pc, bi, true);
        bi->bimodalIndex = bindex(pc);

        DPRINTF(Tage, "Predict for %llx: ptr:%i, GHR:%x bi:%i, Ti[2]:%i, TT[2]:%x\n",
                branch_pc, threadHistory[tid].ptGhist, getGHR(tid),
                bi->bimodalIndex, tableIndices[2], tableTags[2]);
        DPRINTF(Tage, "Folded Hist[1] for %llx: ci:%x ct0:%x ct1:%x\n",
                branch_pc,
                threadHistory[tid].computeIndices[1].comp,
                threadHistory[tid].computeTags[0][1].comp,
                threadHistory[tid].computeTags[1][1].comp);

        bi->hitBank = 0;
        bi->altBank = 0;
        //Look for the bank with longest matching history
        for (int i = nHistoryTables; i > 0; i--) {
            if (noSkip[i] &&
                gtable[i][tableIndices[i]].tag == tableTags[i]) {
                bi->hitBank = i;
                bi->hitBankIndex = tableIndices[bi->hitBank];
                if (gtable[i][tableIndices[i]].pc != pc) {
                    stats.aliasCollision++;
                }
                break;
            }
        }
        //Look for the alternate bank
        for (int i = bi->hitBank - 1; i > 0; i--) {
            if (noSkip[i] &&
                gtable[i][tableIndices[i]].tag == tableTags[i]) {
                bi->altBank = i;
                bi->altBankIndex = tableIndices[bi->altBank];
                if (gtable[i][tableIndices[i]].pc != pc) {
                    stats.aliasCollision++;
                }
                break;
            }
        }
        //computes the prediction and the alternate prediction
        if (bi->hitBank > 0) {
            if (bi->altBank > 0) {
                bi->altTaken =
                    gtable[bi->altBank][tableIndices[bi->altBank]].ctr >= 0;
                extraAltCalc(bi);
            }else {
                bi->altTaken = getBimodePred(pc, bi);
            }

            bi->longestMatchPred =
                gtable[bi->hitBank][tableIndices[bi->hitBank]].ctr >= 0;
            bi->pseudoNewAlloc =
                abs(2 * gtable[bi->hitBank][bi->hitBankIndex].ctr + 1) <= 1;

            //if the entry is recognized as a newly allocated entry and
            //useAltPredForNewlyAllocated is positive use the alternate
            //prediction
            if ((useAltPredForNewlyAllocated[getUseAltIdx(bi, branch_pc)] < 0)
                || ! bi->pseudoNewAlloc) {
                bi->tagePred = bi->longestMatchPred;
                bi->provider = TAGE_LONGEST_MATCH;
            } else {
                bi->tagePred = bi->altTaken;
                bi->provider = bi->altBank ? TAGE_ALT_MATCH
                                           : BIMODAL_ALT_MATCH;
            }
        } else {
            bi->altTaken = getBimodePred(pc, bi);
            bi->tagePred = bi->altTaken;
            bi->longestMatchPred = bi->altTaken;
            bi->provider = BIMODAL_ONLY;
        }
        //end TAGE prediction

        pred_taken = (bi->tagePred);
        DPRINTF(Tage, "Predict for %lx: taken?:%d, tagePred:%d, altPred:%d\n",
                branch_pc, pred_taken, bi->tagePred, bi->altTaken);
    }
    bi->branchPC = branch_pc;
    bi->condBranch = cond_branch;
    return pred_taken;
}


bool
TAGEBase::tageDummyPredict(ThreadID tid, Addr branch_pc,
              bool cond_branch, BranchInfo* bi, bool speculative)
{
    // The prediction is made speculatively there is no chance to make
    // a prediction. We assume its a not taken branch.
    if (speculative) {
        stats.dummyPredictions++;
        if (cond_branch) {
            // TAGE prediction

            // calculateIndicesAndTags(tid, branch_pc, bi);
            bi->bimodalIndex = bindex(branch_pc);
            bi->hitBank = 0;
            bi->altBank = 0;
            bi->altTaken = false;
            bi->tagePred = false;
            bi->longestMatchPred = false;
            bi->provider = BIMODAL_ONLY;
            bi->dummy = true;
            //end TAGE prediction
            DPRINTF(Tage, "Dummy Predict for %lx\n", branch_pc);
        }
        bi->branchPC = branch_pc;
        bi->condBranch = cond_branch;
        return false;
    }

    assert(cond_branch);

    // At commit we have the chance to compute what TAGE would have
    // predicted in the first place. This helps us to update the predictor.
    // We compute the prediction on the commited state!!
    // Forthermore, we only compute the longest hit
    calculateIndicesAndTags(tid, branch_pc, bi, false);
    bi->bimodalIndex = bindex(branch_pc);

    bi->hitBank = 0;
    bi->altBank = 0;
    //Look for the bank with longest matching history
    for (int i = nHistoryTables; i > 0; i--) {
        if (noSkip[i] &&
            gtable[i][tableIndices[i]].tag == tableTags[i]) {
            bi->hitBank = i;
            bi->hitBankIndex = tableIndices[bi->hitBank];
            if (gtable[i][tableIndices[i]].pc != branch_pc) {
                stats.aliasCollision++;
            }
            break;
        }
    }

    //computes the prediction but not the alternate prediction
    if (bi->hitBank > 0) {
        bi->longestMatchPred =
            gtable[bi->hitBank][tableIndices[bi->hitBank]].ctr >= 0;
        bi->pseudoNewAlloc =
            abs(2 * gtable[bi->hitBank][bi->hitBankIndex].ctr + 1) <= 1;

        //if the entry is recognized as a newly allocated entry and
        //useAltPredForNewlyAllocated is positive use the alternate
        //prediction
        if (!bi->pseudoNewAlloc) {
            bi->tagePred = bi->longestMatchPred;
            bi->provider = TAGE_LONGEST_MATCH;
        } else {
            bi->tagePred = getBimodePred(branch_pc, bi);
            bi->provider = BIMODAL_ALT_MATCH;
        }
    } else {
        bi->altTaken = getBimodePred(branch_pc, bi);
        bi->tagePred = bi->altTaken;
        bi->longestMatchPred = bi->altTaken;
        bi->provider = BIMODAL_ONLY;
    }

    DPRINTF(Tage, "Dummy predict (non-spec) for %lx: tagePred:%d [%d,%i], altPred:%d\n",
                branch_pc, bi->tagePred, bi->hitBank, bi->hitBankIndex, bi->altTaken);

    return bi->tagePred;
}

void
TAGEBase::adjustAlloc(bool & alloc, bool taken, bool pred_taken)
{
    // Nothing for this base class implementation
}

void
TAGEBase::handleAllocAndUReset(bool alloc, bool taken, BranchInfo* bi,
                           int nrand)
{
    if (alloc) {
        // is there some "unuseful" entry to allocate
        uint8_t min = 1;
        for (int i = nHistoryTables; i > bi->hitBank; i--) {
            if (gtable[i][bi->tableIndices[i]].u < min) {
                min = gtable[i][bi->tableIndices[i]].u;
            }
        }

        // we allocate an entry with a longer history
        // to  avoid ping-pong, we do not choose systematically the next
        // entry, but among the 3 next entries
        int Y = nrand &
            ((1ULL << (nHistoryTables - bi->hitBank - 1)) - 1);
        int X = bi->hitBank + 1;
        if (Y & 1) {
            X++;
            if (Y & 2)
                X++;
        }
        // No entry available, forces one to be available
        if (min > 0) {
            gtable[X][bi->tableIndices[X]].u = 0;
        }

        //Allocate entries
        unsigned numAllocated = 0;
        for (int i = X; i <= nHistoryTables; i++) {
            if (gtable[i][bi->tableIndices[i]].u == 0) {

                DPRINTF(Tage, "Allocate for %llx: i:%i Tidx:%i, Ttag:%x\n",
                        bi->branchPC, i, bi->tableIndices[i], bi->tableTags[i]);

                stats.allocations[i]++;

                gtable[i][bi->tableIndices[i]].tag = bi->tableTags[i];
                gtable[i][bi->tableIndices[i]].ctr = (taken) ? 0 : -1;
                gtable[i][bi->tableIndices[i]].correct_pred = 0;
                gtable[i][bi->tableIndices[i]].pc = bi->branchPC;
                ++numAllocated;
                if (numAllocated == maxNumAlloc) {
                    break;
                }
            }
        }
    }

    tCounter++;

    handleUReset();
}

void
TAGEBase::handleUReset()
{
    //periodic reset of u: reset is not complete but bit by bit
    if ((tCounter & ((1ULL << logUResetPeriod) - 1)) == 0) {
        // reset least significant bit
        // most significant bit becomes least significant bit
        for (int i = 1; i <= nHistoryTables; i++) {
            for (int j = 0; j < (1ULL << logTagTableSizes[i]); j++) {
                resetUctr(gtable[i][j].u);
            }
        }
        stats.uResets++;
    }
}

void
TAGEBase::resetUctr(uint8_t & u)
{
    u >>= 1;
}

void
TAGEBase::condBranchUpdate(ThreadID tid, Addr branch_pc, bool taken,
    BranchInfo* bi, int nrand, Addr corrTarget, bool pred, bool preAdjustAlloc)
{

    // For a dummy prediction we did not make a tage prediction in the first
    // place. Now we have the chance to compute what TAGE would have predicted
    // to update the prediction
    if (bi->dummy) {

        stats.dummyPredictionsCommitted++;
        if (bi->tagePred == taken) {
            stats.dummyPredictionsCorrect++;
        } else {
            stats.dummyPredictionsWrong++;
        }

        if (allowLatePredict) {
            tageDummyPredict(tid, branch_pc, true, bi, false);

            if (bi->tagePred == taken) {
                stats.dummyPredictionsLongestWouldHaveHit++;
            }

        } else if (!noDummyAllocate) {
            calculateIndicesAndTags(tid, branch_pc, bi, false);
        }
    }


    // TAGE UPDATE
    // try to allocate a  new entries only if prediction was wrong
    bool alloc = (bi->tagePred != taken) && (bi->hitBank < nHistoryTables);






    // For a dummy predication we could not make the calculation
    // for tags and indices. Therefore we need to make it now from the
    // commited state!
    if (bi->dummy && noDummyAllocate) {
        alloc = false;
    }




    if (preAdjustAlloc) {
        adjustAlloc(alloc, taken, pred);
    }

    if (bi->hitBank > 0) {
        // Manage the selection between longest matching and alternate
        // matching for "pseudo"-newly allocated longest matching entry
        bool PseudoNewAlloc = bi->pseudoNewAlloc;
        // an entry is considered as newly allocated if its prediction
        // counter is weak
        if (PseudoNewAlloc) {
            if (bi->longestMatchPred == taken) {
                alloc = false;
            }
            // if it was delivering the correct prediction, no need to
            // allocate new entry even if the overall prediction was false
            if (bi->longestMatchPred != bi->altTaken) {
                ctrUpdate(
                    useAltPredForNewlyAllocated[getUseAltIdx(bi, branch_pc)],
                    bi->altTaken == taken, useAltOnNaBits);
            }
        }
    }

    if (!preAdjustAlloc) {
        adjustAlloc(alloc, taken, pred);
    }

    DPRINTF(Tage, "%s(PC:%lx, taken:%i, target:%#x, pred:%i): Tage Pred:[%d,%i], tag:%lx, Bi:%d, "
                    "Tage:%i, dummy:%i, correct:%i, alloc%i \n", __func__,
                branch_pc, taken, corrTarget, pred,
                bi->hitBank, bi->hitBankIndex, bi->tableTags[bi->hitBank], bi->bimodalIndex,
                bi->tagePred, bi->dummy, (bi->tagePred == taken), alloc);


    handleAllocAndUReset(alloc, taken, bi, nrand);

    handleTAGEUpdate(branch_pc, taken, bi);
}

void
TAGEBase::handleTAGEUpdate(Addr branch_pc, bool taken, BranchInfo* bi)
{
    if (bi->hitBank > 0) {

        DPRINTF(Tage, "%s PC:%lx: Updating table entry [%d,%i] tag:%lx\n", __func__,
                branch_pc, bi->hitBank, bi->hitBankIndex, bi->tableTags[bi->hitBank]);
        DPRINTF(Tage, "PC:%lx: Recompt: idx:%i, Tag:%lx, GHR:%lx\n",
                branch_pc, gindex(0,branch_pc,bi->hitBank), gtag(0,branch_pc,bi->hitBank), getGHR(0));

        ctrUpdate(gtable[bi->hitBank][bi->hitBankIndex].ctr, taken,
                  tagTableCounterBits);

        stats.ctrUpdates[bi->hitBank]++;
        // if the provider entry is not certified to be useful also update
        // the alternate prediction
        if (gtable[bi->hitBank][bi->hitBankIndex].u == 0) {
            if (bi->altBank > 0) {
                ctrUpdate(gtable[bi->altBank][bi->altBankIndex].ctr, taken,
                          tagTableCounterBits);
                DPRINTF(Tage, "Updating tag table entry (%d,%d) for"
                        " branch %lx\n", bi->hitBank, bi->hitBankIndex,
                        branch_pc);
            }
            if (bi->altBank == 0) {
                baseUpdate(branch_pc, taken, bi);
            }
        }

        if (bi->tagePred == taken) {
            gtable[bi->hitBank][bi->hitBankIndex].correct_pred++;


        }

        DPRINTF(Tage, "%s: Tag(%d,%d) for branch %lx was: %s. "
                    "Used already: %d direction:%i, ctr:%i\n", __func__,
                    bi->hitBank, bi->hitBankIndex, branch_pc,
                    (bi->tagePred == taken) ? "correct" : "wrong",
                    gtable[bi->hitBank][bi->hitBankIndex].correct_pred,
                    taken, gtable[bi->hitBank][bi->hitBankIndex].ctr);

        // update the u counter
        if (bi->tagePred != bi->altTaken) {
            DPRINTF(Tage, "TAGE correct - TT:[%d,%d] PC:%llx\n",
                        bi->hitBank, bi->hitBankIndex, branch_pc);
            unsignedCtrUpdate(gtable[bi->hitBank][bi->hitBankIndex].u,
                              bi->tagePred == taken, tagTableUBits);
        }
    } else {
        baseUpdate(branch_pc, taken, bi);
        DPRINTF(Tage, "%s: Tag(%d,%d) for branch %lx was: %s. "
                    "Used already: zz direction:%i\n", __func__,
                    bi->hitBank, bi->hitBankIndex, branch_pc,
                    (bi->tagePred == taken) ? "correct" : "wrong");
    }
}

void
TAGEBase::updateHistories(ThreadID tid, Addr branch_pc, bool taken,
                          BranchInfo* bi, bool speculative,
                          const StaticInstPtr &inst, Addr target)
{
    uint64_t targethash = (branch_pc >> 2) ^ (target >> 3);
    ThreadHistory &tHist = threadHistory[tid];

    // Update the commited history pointer
    if (speculative != speculativeHistUpdate) {

        if (bi->modifiedHist) {
            threadHistory[tid].commitPtGhist -= takenOnlyHistory ? 2 : 1;
            uint8_t * commitGhist = &tHist.globalHistory[tHist.commitPtGhist];

            // Update the non-speculative folded histories
            for (int i = 1; i <= nHistoryTables; i++)
            {
                if (takenOnlyHistory && taken) {
                    tHist.commitComputeIndices[i].update(&(commitGhist[1]));
                    tHist.commitComputeTags[0][i].update(&(commitGhist[1]));
                    tHist.commitComputeTags[1][i].update(&(commitGhist[1]));
                }
                if (!takenOnlyHistory || taken) {
                    // assert(bi->modifiedHist); // TODO: remove the assert
                    tHist.commitComputeIndices[i].update(commitGhist);
                    tHist.commitComputeTags[0][i].update(commitGhist);
                    tHist.commitComputeTags[1][i].update(commitGhist);
                }
            }
        }
        DPRINTF(Tage, "%s(commit): PC:%#llx, T:%i/%#llx > TH:%llx, ptr:%d GHR:%llx, [bi:%x,ti:%x,tt:%x] > [ci:%x,ct0:%x,ct1:%x]\n", __func__,
                branch_pc, taken, target, targethash, threadHistory[tid].commitPtGhist, getGHR(tid,false),
                bi->bimodalIndex, bi->tableIndices[2], bi->tableTags[2],
                threadHistory[tid].commitComputeIndices[1].comp,
                threadHistory[tid].commitComputeTags[0][1].comp,
                threadHistory[tid].commitComputeTags[1][1].comp);
        // DPRINTF(Tage, "%s(commit): PC:%#llx, T:%i/%#llx > TH:%llx, ptr:%d GHR:%llx, [bi:%x,ti:%x,tt:%x] > [ci:%x,ct0:%x,ct1:%x]\n", __func__,
        //         branch_pc, taken, target, targethash, threadHistory[tid].commitPtGhist, getGHR(tid,false),
        //         bi->bimodalIndex, bi->tableIndices[2], bi->tableTags[2],
        //         threadHistory[tid].computeIndices[1].comp,
        //         threadHistory[tid].computeTags[0][1].comp,
        //         threadHistory[tid].computeTags[1][1].comp);
        return;
    }






    DPRINTF(Tage, "%s(speculative): PC:%#llx, T:%i/%#llx > TH:%llx, ptr:%d GHR:%llx\n", __func__,
                    branch_pc, taken, target, targethash, threadHistory[tid].ptGhist, getGHR(tid,true));


    // In case we are going to modify the history speculatively
    // remember the current state
    if (speculative && (!takenOnlyHistory || taken)) {
        bi->modifiedHist = true;
        recordHistState(tid, bi);
    }


    //  UPDATE path HISTORIES
    bool pathbit = ((branch_pc >> instShiftAmt) & 1);
    tHist.pathHist = (tHist.pathHist << 1) + pathbit;
    tHist.pathHist = (tHist.pathHist & ((1ULL << pathHistBits) - 1));

    // on a squash, return pointers to this and recompute indices.
    // update user history

    if (takenOnlyHistory) {

        updateGHist(tHist.gHist, taken, branch_pc, target,
                    tHist.globalHistory, tHist.ptGhist);
    } else {
        updateGHist(tHist.gHist, taken,
                    tHist.globalHistory, tHist.ptGhist);

        // TODO: make the update function different
        // Taken and direction history should be the same.
        if (speculative) {
            bi->ptGhist = tHist.ptGhist;
            bi->pathHist = tHist.pathHist;
        }
    }

    DPRINTF(Tage, "After update GHR for PC:%lx; ptr:%d, GHR:%llx\n",
                    branch_pc, tHist.ptGhist, getGHR(tid));

    //prepare next index and tag computations for user branchs
    for (int i = 1; i <= nHistoryTables; i++)
    {
        if (takenOnlyHistory && taken) {
            tHist.computeIndices[i].update(&(tHist.gHist[1]));
            tHist.computeTags[0][i].update(&(tHist.gHist[1]));
            tHist.computeTags[1][i].update(&(tHist.gHist[1]));
        }
        if (!takenOnlyHistory || taken) {
            assert(bi->modifiedHist || !speculativeHistUpdate); // TODO: remove the assert
            tHist.computeIndices[i].update(tHist.gHist);
            tHist.computeTags[0][i].update(tHist.gHist);
            tHist.computeTags[1][i].update(tHist.gHist);
        }
    }



    DPRINTF(Tage, "Updating global histories with branch:%lx; taken?:%d, "
            "path Hist: %x; pointer:%d, GHR:%llx\n", branch_pc, taken,
            tHist.pathHist, tHist.ptGhist, getGHR(tid));
    assert(threadHistory[tid].gHist ==
            &threadHistory[tid].globalHistory[threadHistory[tid].ptGhist]);
}

void
TAGEBase::squash(ThreadID tid, bool taken, TAGEBase::BranchInfo *bi,
                 Addr target)
{
    if (!speculativeHistUpdate) {
        /* If there are no speculative updates, no actions are needed */
        return;
    }

    ThreadHistory& tHist = threadHistory[tid];

    // In case the history was modified
    // revert to the global history pointers
    // and the folded histories to the previous state.
    if (bi->modifiedHist) {
        restoreHistState(tid, bi);
        bi->modifiedHist = false;

    }

    // // Recalculate the Indices
    // if (bi->condBranch) {
    //     calculateIndicesAndTags(tid, bi->branchPC, bi);
    // }


    // In case we are going to modify the history
    // remember the current state
    if (!bi->modifiedHist && (!takenOnlyHistory || taken)) {

        bi->modifiedHist = true;
        recordHistState(tid, bi);
    }


    DPRINTF(Tage, "Before squash upd. GHR for PC:%lx; ptr:%d, GHR:%llx\n",
                    bi->branchPC, tHist.ptGhist, getGHR(tid));

    if (takenOnlyHistory) {
        // Only update taken branches

        updateGHist(tHist.gHist, taken, bi->branchPC, target,
                    tHist.globalHistory, tHist.ptGhist);

    } else {
        // tHist.gHist[0] = (taken ? 1 : 0);
        updateGHist(tHist.gHist, taken,
                    tHist.globalHistory, tHist.ptGhist);
    }

    DPRINTF(Tage, "After squash GHR for PC:%lx; ptr:%d, GHR:%llx\n",
                    bi->branchPC, tHist.ptGhist, getGHR(tid));

    for (int i = 1; i <= nHistoryTables; i++) {
        // For taken history we shift two bits
        // into the global history in case the branch was taken
        if (takenOnlyHistory && taken) {
            tHist.computeIndices[i].update(&(tHist.gHist[1]));
            tHist.computeTags[0][i].update(&(tHist.gHist[1]));
            tHist.computeTags[1][i].update(&(tHist.gHist[1]));
        }
        if (!takenOnlyHistory || taken) {
            tHist.computeIndices[i].update(tHist.gHist);
            tHist.computeTags[0][i].update(tHist.gHist);
            tHist.computeTags[1][i].update(tHist.gHist);
        }
    }

    DPRINTF(Tage, "Restoring branch info: PC:%llx; taken? %d; PathHistory:%x,"
            " pointer:%d, GHR:%llx\n", bi->branchPC, taken,
            bi->pathHist, tHist.ptGhist, getGHR(tid));
}

void
TAGEBase::btbUpdate(ThreadID tid, Addr branch_pc, BranchInfo* &bi)
{
    // For taken only history we do not care about
    if (speculativeHistUpdate) {
        ThreadHistory& tHist = threadHistory[tid];
        assert(tHist.gHist == &tHist.globalHistory[tHist.ptGhist]);

        // if (takenOnlyHistory) {
        //     // For taken only revert the taken history
        //     tHist.pathHist = bi->pathHist;
        //     tHist.ptGhist = bi->ptGhist;
        //     tHist.gHist = &(tHist.globalHistory[tHist.ptGhist]);
        // } else {
        //     tHist.gHist[0] = 0;
        // }

        // for (int i = 1; i <= nHistoryTables; i++) {
        //     tHist.computeIndices[i].comp = bi->ci[i];
        //     tHist.computeTags[0][i].comp = bi->ct0[i];
        //     tHist.computeTags[1][i].comp = bi->ct1[i];

        //     // As its not taken do not update takenOnly history
        //     if (!takenOnlyHistory) {
        //         tHist.computeIndices[i].update(tHist.gHist);
        //         tHist.computeTags[0][i].update(tHist.gHist);
        //         tHist.computeTags[1][i].update(tHist.gHist);
        //     }
        // }

    // DPRINTF(Tage, "After squash GHR for PC:%lx; ptr:%d, GHR:%llx\n",
    //                 bi->branchPC, tHist.ptGhist, getGHR(tid));



    //     for (int i = 1; i <= nHistoryTables; i++) {
    //         tHist.computeIndices[i].comp = bi->ci[i];
    //         tHist.computeTags[0][i].comp = bi->ct0[i];
    //         tHist.computeTags[1][i].comp = bi->ct1[i];

    //         // As its not taken do not update takenOnly history
    //         if (!takenOnlyHistory) {
    //             tHist.computeIndices[i].update(tHist.gHist);
    //             tHist.computeTags[0][i].update(tHist.gHist);
    //             tHist.computeTags[1][i].update(tHist.gHist);
    //         }
    //     }


        // In case the history was modified
        // revert to the global history pointers
        // and the folded histories to the previous state.
        if (bi->modifiedHist) {
            restoreHistState(tid, bi);
            bi->modifiedHist = false;
        }

        if (!takenOnlyHistory) {

            updateGHist(tHist.gHist, 0,
                    tHist.globalHistory, tHist.ptGhist);

            for (int i = 1; i <= nHistoryTables; i++) {

                tHist.computeIndices[i].update(tHist.gHist);
                tHist.computeTags[0][i].update(tHist.gHist);
                tHist.computeTags[1][i].update(tHist.gHist);
            }
        }


        DPRINTF(Tage, "BTB miss resets prediction: %lx: ptr:%i\n",
                    branch_pc, tHist.ptGhist);

        DPRINTF(Tage, "Folded Hist[1] for %llx: ci:%x ct0:%x ct1:%x\n",
                branch_pc,
                threadHistory[tid].computeIndices[1].comp,
                threadHistory[tid].computeTags[0][1].comp,
                threadHistory[tid].computeTags[1][1].comp);
    }
}

void
TAGEBase::recordHistState(ThreadID tid, BranchInfo* bi)
{

    ThreadHistory& tHist = threadHistory[tid];
    bi->ptGhist = tHist.ptGhist;
    bi->pathHist = tHist.pathHist;

    for (int i = 1; i <= nHistoryTables; i++) {
        bi->ci[i]  = tHist.computeIndices[i].comp;
        bi->ct0[i] = tHist.computeTags[0][i].comp;
        bi->ct1[i] = tHist.computeTags[1][i].comp;
    }

    DPRINTF(Tage, "%s: PC:%llx, Gptr:%d, GHR:%llx, c[2][i:%x,t0:%x,t1:%x]\n",
                __func__, bi->branchPC, tHist.ptGhist, getGHR(tid),
                threadHistory[tid].computeIndices[1].comp,
                threadHistory[tid].computeTags[0][1].comp,
                threadHistory[tid].computeTags[1][1].comp);
}

void
TAGEBase::restoreHistState(ThreadID tid, BranchInfo* bi)
{
    if (!bi->modifiedHist) {
        return;
    }

    ThreadHistory& tHist = threadHistory[tid];
    // tHist.pathHist = bi->pathHist;
    // tHist.ptGhist = bi->ptGhist;
    // tHist.gHist = &(tHist.globalHistory[tHist.ptGhist]);

    // for (int i = 1; i <= nHistoryTables; i++) {
    //     tHist.computeIndices[i].comp = bi->ci[i];
    //     tHist.computeTags[0][i].comp = bi->ct0[i];
    //     tHist.computeTags[1][i].comp = bi->ct1[i];
    // }

    for (int i = 1; i <= nHistoryTables; i++) {

        tHist.computeIndices[i].restore(tHist.gHist);
        tHist.computeTags[0][i].restore(tHist.gHist);
        tHist.computeTags[1][i].restore(tHist.gHist);


        if (takenOnlyHistory) {
            tHist.computeIndices[i].restore(&(tHist.gHist[1]));
            tHist.computeTags[0][i].restore(&(tHist.gHist[1]));
            tHist.computeTags[1][i].restore(&(tHist.gHist[1]));
        }
    }


    if (takenOnlyHistory) {
        tHist.ptGhist += 2;
        tHist.gHist += 2;
    } else {
        tHist.ptGhist += 1;
        tHist.gHist += 1;
    }
    //     // tHist.pathHist = bi->pathHist;
    // //  =
    // bi->ptGhist = tHist.ptGhist;
    tHist.gHist = &(tHist.globalHistory[tHist.ptGhist]);


    calculateIndicesAndTags(tid, bi->branchPC, bi, true);



    DPRINTF(Tage, "%s: PC:%llx, Gptr:%d, GHR:%llx, c[2][i:%x,t0:%x,t1:%x]\n",
                __func__, bi->branchPC, tHist.ptGhist, getGHR(tid),
                threadHistory[tid].computeIndices[1].comp,
                threadHistory[tid].computeTags[0][1].comp,
                threadHistory[tid].computeTags[1][1].comp);
}

void
TAGEBase::extraAltCalc(BranchInfo* bi)
{
    // do nothing. This is only used in some derived classes
    return;
}

void
TAGEBase::updateStats(bool taken, BranchInfo* bi)
{
    if (taken == bi->tagePred) {
        // correct prediction
        switch (bi->provider) {
          case BIMODAL_ONLY: stats.bimodalProviderCorrect++; break;
          case TAGE_LONGEST_MATCH: stats.longestMatchProviderCorrect++; break;
          case BIMODAL_ALT_MATCH:
            stats.bimodalAltMatchProviderCorrect++;
            break;
          case TAGE_ALT_MATCH: stats.altMatchProviderCorrect++; break;
        }
    } else {
        // wrong prediction
        switch (bi->provider) {
          case BIMODAL_ONLY: stats.bimodalProviderWrong++; break;
          case TAGE_LONGEST_MATCH:
            stats.longestMatchProviderWrong++;
            if (bi->altTaken == taken) {
                stats.altMatchProviderWouldHaveHit++;
            }
            break;
          case BIMODAL_ALT_MATCH:
            stats.bimodalAltMatchProviderWrong++;
            break;
          case TAGE_ALT_MATCH:
            stats.altMatchProviderWrong++;
            break;
        }

        switch (bi->provider) {
          case BIMODAL_ALT_MATCH:
          case TAGE_ALT_MATCH:
            if (bi->longestMatchPred == taken) {
                stats.longestMatchProviderWouldHaveHit++;
            }
        }
    }

    switch (bi->provider) {
      case TAGE_LONGEST_MATCH:
      case TAGE_ALT_MATCH:
        stats.longestMatchProvider[bi->hitBank]++;
        stats.altMatchProvider[bi->altBank]++;
        break;
    }
}

unsigned
TAGEBase::getGHR(ThreadID tid, BranchInfo *bi) const
{
    unsigned val = 0;
    for (unsigned i = 0; i < 16; i++) {
        // Make sure we don't go out of bounds
        int gh_offset = bi->ptGhist + i;
        assert(&(threadHistory[tid].globalHistory[gh_offset]) <
               threadHistory[tid].globalHistory + histBufferSize);
        val |= ((threadHistory[tid].globalHistory[gh_offset] & 0x1) << i);
    }
    return val;
}

unsigned
TAGEBase::getGHR(ThreadID tid, bool speculative) const
{
    unsigned val = 0;
    int gh_ptr = speculative ? threadHistory[tid].ptGhist
                             : threadHistory[tid].commitPtGhist;
    for (unsigned i = 0; i < 16; i++) {
        // Make sure we don't go out of bounds
        assert(&(threadHistory[tid].globalHistory[gh_ptr + i]) <
               threadHistory[tid].globalHistory + histBufferSize);
        val |= ((threadHistory[tid].globalHistory[gh_ptr + i] & 0x1) << i);
    }
    return val;
}

std::string
TAGEBase::getGHRStr(ThreadID tid, BranchInfo *bi) const
{
    std::ostringstream oss;

    for (unsigned i = 0; i < 16; i++) {
        // Make sure we don't go out of bounds
        int gh_offset = bi->ptGhist + i;
        assert(&(threadHistory[tid].globalHistory[gh_offset]) <
               threadHistory[tid].globalHistory + histBufferSize);
        oss << (threadHistory[tid].globalHistory[gh_offset] & 0x1);
    }
    return oss.str();
}

TAGEBase::TAGEBaseStats::TAGEBaseStats(
    statistics::Group *parent, unsigned nHistoryTables)
    : statistics::Group(parent),
      ADD_STAT(longestMatchProviderCorrect, statistics::units::Count::get(),
               "Number of times TAGE Longest Match is the provider and the "
               "prediction is correct"),
      ADD_STAT(altMatchProviderCorrect, statistics::units::Count::get(),
               "Number of times TAGE Alt Match is the provider and the "
               "prediction is correct"),
      ADD_STAT(bimodalAltMatchProviderCorrect, statistics::units::Count::get(),
               "Number of times TAGE Alt Match is the bimodal and it is the "
               "provider and the prediction is correct"),
      ADD_STAT(bimodalProviderCorrect, statistics::units::Count::get(),
               "Number of times there are no hits on the TAGE tables and the "
               "bimodal prediction is correct"),
      ADD_STAT(longestMatchProviderWrong, statistics::units::Count::get(),
               "Number of times TAGE Longest Match is the provider and the "
               "prediction is wrong"),
      ADD_STAT(altMatchProviderWrong, statistics::units::Count::get(),
               "Number of times TAGE Alt Match is the provider and the "
               "prediction is wrong"),
      ADD_STAT(bimodalAltMatchProviderWrong, statistics::units::Count::get(),
               "Number of times TAGE Alt Match is the bimodal and it is the "
               "provider and the prediction is wrong"),
      ADD_STAT(bimodalProviderWrong, statistics::units::Count::get(),
               "Number of times there are no hits on the TAGE tables and the "
               "bimodal prediction is wrong"),
      ADD_STAT(altMatchProviderWouldHaveHit, statistics::units::Count::get(),
               "Number of times TAGE Longest Match is the provider, the "
               "prediction is wrong and Alt Match prediction was correct"),
      ADD_STAT(longestMatchProviderWouldHaveHit, statistics::units::Count::get(),
               "Number of times TAGE Alt Match is the provider, the "
               "prediction is wrong and Longest Match prediction was correct"),
      ADD_STAT(uResets, statistics::units::Count::get(),
               "Number of global reset of the useful bits was performed."),
      ADD_STAT(aliasCollision, statistics::units::Count::get(),
               "Number of times a prediction was made from an entry that "
               "did not belong to a branch but the TAG matched."),


ADD_STAT(dummyPredictions, statistics::units::Count::get(),
               "Number of global reset of the useful bits was performed."),
ADD_STAT(dummyPredictionsCommitted, statistics::units::Count::get(),
               "Number of global reset of the useful bits was performed."),
ADD_STAT(dummyPredictionsCorrect, statistics::units::Count::get(),
               "Number of global reset of the useful bits was performed."),
ADD_STAT(dummyPredictionsWrong, statistics::units::Count::get(),
               "Number of global reset of the useful bits was performed."),
ADD_STAT(dummyPredictionsLongestWouldHaveHit, statistics::units::Count::get(),
               "Number of global reset of the useful bits was performed."),



      ADD_STAT(longestMatchProvider, statistics::units::Count::get(),
               "TAGE provider for longest match"),
      ADD_STAT(altMatchProvider, statistics::units::Count::get(),
               "TAGE provider for alt match"),

               ADD_STAT(allocations, statistics::units::Count::get(),
               "TAGE provider for alt match"),
               ADD_STAT(ctrUpdates, statistics::units::Count::get(),
               "TAGE provider for alt match")
{
    longestMatchProvider.init(nHistoryTables + 1).flags(statistics::total);
    altMatchProvider.init(nHistoryTables + 1).flags(statistics::total);
    allocations.init(nHistoryTables + 1).flags(statistics::total);
    ctrUpdates.init(nHistoryTables + 1).flags(statistics::total);

}

int8_t
TAGEBase::getCtr(int hitBank, int hitBankIndex) const
{
    return gtable[hitBank][hitBankIndex].ctr;
}

unsigned
TAGEBase::getTageCtrBits() const
{
    return tagTableCounterBits;
}

int
TAGEBase::getPathHist(ThreadID tid) const
{
    return threadHistory[tid].pathHist;
}

bool
TAGEBase::isSpeculativeUpdateEnabled() const
{
    return speculativeHistUpdate;
}

size_t
TAGEBase::getSizeInBits() const {
    size_t bits = 0;
    for (int i = 1; i <= nHistoryTables; i++) {
        bits += (1 << logTagTableSizes[i]) *
            (tagTableCounterBits + tagTableUBits + tagTableTagWidths[i]);
    }
    uint64_t bimodalTableSize = 1ULL << logTagTableSizes[0];
    bits += numUseAltOnNa * useAltOnNaBits;
    bits += bimodalTableSize;
    bits += (bimodalTableSize >> logRatioBiModalHystEntries);
    bits += histLengths[nHistoryTables];
    bits += pathHistBits;
    bits += logUResetPeriod;
    return bits;
}

} // namespace branch_prediction
} // namespace gem5
