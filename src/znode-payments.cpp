// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activeznode.h"
#include "darksend.h"
#include "znode-payments.h"
#include "znode-sync.h"
#include "znodeman.h"
#include "netfulfilledman.h"
#include "spork.h"
#include "util.h"

#include <boost/lexical_cast.hpp>

/** Object for who's going to get paid on which blocks */
CEnodePayments mnpayments;

CCriticalSection cs_vecPayees;
CCriticalSection cs_mapEnodeBlocks;
CCriticalSection cs_mapEnodePaymentVotes;

/**
* IsBlockValueValid
*
*   Determine if coinbase outgoing created money is the correct value
*
*   Why is this needed?
*   - In Dash some blocks are superblocks, which output much higher amounts of coins
*   - Otherblocks are 10% lower in outgoing value, so in total, no extra coins are created
*   - When non-superblocks are detected, the normal schedule should be maintained
*/

bool IsBlockValueValid(const CBlock &block, int nBlockHeight, CAmount blockReward, std::string &strErrorRet) {
    strErrorRet = "";

    bool isBlockRewardValueMet = (block.vtx[0].GetValueOut() <= blockReward);
    if (fDebug) LogPrintf("block.vtx[0].GetValueOut() %lld <= blockReward %lld\n", block.vtx[0].GetValueOut(), blockReward);

    // we are still using budgets, but we have no data about them anymore,
    // all we know is predefined budget cycle and window

//    const Consensus::Params &consensusParams = Params().GetConsensus();
//
////    if (nBlockHeight < consensusParams.nSuperblockStartBlock) {
//        int nOffset = nBlockHeight % consensusParams.nBudgetPaymentsCycleBlocks;
//        if (nBlockHeight >= consensusParams.nBudgetPaymentsStartBlock &&
//            nOffset < consensusParams.nBudgetPaymentsWindowBlocks) {
//            // NOTE: make sure SPORK_13_OLD_SUPERBLOCK_FLAG is disabled when 12.1 starts to go live
//            if (znodeSync.IsSynced() && !sporkManager.IsSporkActive(SPORK_13_OLD_SUPERBLOCK_FLAG)) {
//                // no budget blocks should be accepted here, if SPORK_13_OLD_SUPERBLOCK_FLAG is disabled
//                LogPrint("gobject", "IsBlockValueValid -- Client synced but budget spork is disabled, checking block value against block reward\n");
//                if (!isBlockRewardValueMet) {
//                    strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, budgets are disabled",
//                                            nBlockHeight, block.vtx[0].GetValueOut(), blockReward);
//                }
//                return isBlockRewardValueMet;
//            }
//            LogPrint("gobject", "IsBlockValueValid -- WARNING: Skipping budget block value checks, accepting block\n");
//            // TODO: reprocess blocks to make sure they are legit?
//            return true;
//        }
//        // LogPrint("gobject", "IsBlockValueValid -- Block is not in budget cycle window, checking block value against block reward\n");
//        if (!isBlockRewardValueMet) {
//            strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, block is not in budget cycle window",
//                                    nBlockHeight, block.vtx[0].GetValueOut(), blockReward);
//        }
//        return isBlockRewardValueMet;
//    }

    // superblocks started

//    CAmount nSuperblockMaxValue =  blockReward + CSuperblock::GetPaymentsLimit(nBlockHeight);
//    bool isSuperblockMaxValueMet = (block.vtx[0].GetValueOut() <= nSuperblockMaxValue);
//    bool isSuperblockMaxValueMet = false;

//    LogPrint("gobject", "block.vtx[0].GetValueOut() %lld <= nSuperblockMaxValue %lld\n", block.vtx[0].GetValueOut(), nSuperblockMaxValue);

    if (!znodeSync.IsSynced()) {
        // not enough data but at least it must NOT exceed superblock max value
//        if(CSuperblock::IsValidBlockHeight(nBlockHeight)) {
//            if(fDebug) LogPrintf("IsBlockPayeeValid -- WARNING: Client not synced, checking superblock max bounds only\n");
//            if(!isSuperblockMaxValueMet) {
//                strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded superblock max value",
//                                        nBlockHeight, block.vtx[0].GetValueOut(), nSuperblockMaxValue);
//            }
//            return isSuperblockMaxValueMet;
//        }
        if (!isBlockRewardValueMet) {
            strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, only regular blocks are allowed at this height",
                                    nBlockHeight, block.vtx[0].GetValueOut(), blockReward);
        }
        // it MUST be a regular block otherwise
        return isBlockRewardValueMet;
    }

    // we are synced, let's try to check as much data as we can

    if (sporkManager.IsSporkActive(SPORK_9_SUPERBLOCKS_ENABLED)) {
////        if(CSuperblockManager::IsSuperblockTriggered(nBlockHeight)) {
////            if(CSuperblockManager::IsValid(block.vtx[0], nBlockHeight, blockReward)) {
////                LogPrint("gobject", "IsBlockValueValid -- Valid superblock at height %d: %s", nBlockHeight, block.vtx[0].ToString());
////                // all checks are done in CSuperblock::IsValid, nothing to do here
////                return true;
////            }
////
////            // triggered but invalid? that's weird
////            LogPrintf("IsBlockValueValid -- ERROR: Invalid superblock detected at height %d: %s", nBlockHeight, block.vtx[0].ToString());
////            // should NOT allow invalid superblocks, when superblocks are enabled
////            strErrorRet = strprintf("invalid superblock detected at height %d", nBlockHeight);
////            return false;
////        }
//        LogPrint("gobject", "IsBlockValueValid -- No triggered superblock detected at height %d\n", nBlockHeight);
//        if(!isBlockRewardValueMet) {
//            strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, no triggered superblock detected",
//                                    nBlockHeight, block.vtx[0].GetValueOut(), blockReward);
//        }
    } else {
//        // should NOT allow superblocks at all, when superblocks are disabled
        LogPrint("gobject", "IsBlockValueValid -- Superblocks are disabled, no superblocks allowed\n");
        if (!isBlockRewardValueMet) {
            strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, superblocks are disabled",
                                    nBlockHeight, block.vtx[0].GetValueOut(), blockReward);
        }
    }

    // it MUST be a regular block
    return isBlockRewardValueMet;
}

bool IsBlockPayeeValid(const CTransaction &txNew, int nBlockHeight, CAmount blockReward) {
    // we can only check znode payment /
    const Consensus::Params &consensusParams = Params().GetConsensus();

    if (nBlockHeight < consensusParams.nEnodePaymentsStartBlock) {
        //there is no budget data to use to check anything, let's just accept the longest chain
        if (fDebug) LogPrintf("IsBlockPayeeValid -- znode isn't start\n");
        return true;
    }
    if (!znodeSync.IsSynced()) {
        //there is no budget data to use to check anything, let's just accept the longest chain
        if (fDebug) LogPrintf("IsBlockPayeeValid -- WARNING: Client not synced, skipping block payee checks\n");
        return true;
    }

    //check for znode payee
    if (mnpayments.IsTransactionValid(txNew, nBlockHeight)) {
        LogPrint("mnpayments", "IsBlockPayeeValid -- Valid znode payment at height %d: %s", nBlockHeight, txNew.ToString());
        return true;
    } else {
        if(sporkManager.IsSporkActive(SPORK_8_ENODE_PAYMENT_ENFORCEMENT)){
            return false;
        } else {
            LogPrintf("ZNode payment enforcement is disabled, accepting block\n");
            return true;
        }
    }
}

void FillBlockPayments(CMutableTransaction &txNew, int nBlockHeight, CAmount znodePayment, CTxOut &txoutEnodeRet, std::vector <CTxOut> &voutSuperblockRet) {
    // only create superblocks if spork is enabled AND if superblock is actually triggered
    // (height should be validated inside)
//    if(sporkManager.IsSporkActive(SPORK_9_SUPERBLOCKS_ENABLED) &&
//        CSuperblockManager::IsSuperblockTriggered(nBlockHeight)) {
//            LogPrint("gobject", "FillBlockPayments -- triggered superblock creation at height %d\n", nBlockHeight);
//            CSuperblockManager::CreateSuperblock(txNew, nBlockHeight, voutSuperblockRet);
//            return;
//    }

    // FILL BLOCK PAYEE WITH ENODE PAYMENT OTHERWISE
    mnpayments.FillBlockPayee(txNew, nBlockHeight, znodePayment, txoutEnodeRet);
    LogPrint("mnpayments", "FillBlockPayments -- nBlockHeight %d znodePayment %lld txoutEnodeRet %s txNew %s",
             nBlockHeight, znodePayment, txoutEnodeRet.ToString(), txNew.ToString());
}

std::string GetRequiredPaymentsString(int nBlockHeight) {
    // IF WE HAVE A ACTIVATED TRIGGER FOR THIS HEIGHT - IT IS A SUPERBLOCK, GET THE REQUIRED PAYEES
//    if(CSuperblockManager::IsSuperblockTriggered(nBlockHeight)) {
//        return CSuperblockManager::GetRequiredPaymentsString(nBlockHeight);
//    }

    // OTHERWISE, PAY ENODE
    return mnpayments.GetRequiredPaymentsString(nBlockHeight);
}

void CEnodePayments::Clear() {
    LOCK2(cs_mapEnodeBlocks, cs_mapEnodePaymentVotes);
    mapEnodeBlocks.clear();
    mapEnodePaymentVotes.clear();
}

bool CEnodePayments::CanVote(COutPoint outEnode, int nBlockHeight) {
    LOCK(cs_mapEnodePaymentVotes);

    if (mapEnodesLastVote.count(outEnode) && mapEnodesLastVote[outEnode] == nBlockHeight) {
        return false;
    }

    //record this znode voted
    mapEnodesLastVote[outEnode] = nBlockHeight;
    return true;
}

std::string CEnodePayee::ToString() const {
    CTxDestination address1;
    ExtractDestination(scriptPubKey, address1);
    CBitcoinAddress address2(address1);
    std::string str;
    str += "(address: ";
    str += address2.ToString();
    str += ")\n";
    return str;
}

/**
*   FillBlockPayee
*
*   Fill Enode ONLY payment block
*/

void CEnodePayments::FillBlockPayee(CMutableTransaction &txNew, int nBlockHeight, CAmount znodePayment, CTxOut &txoutEnodeRet) {
    // make sure it's not filled yet
    txoutEnodeRet = CTxOut();

    CScript payee;
    bool foundMaxVotedPayee = true;

    if (!mnpayments.GetBlockPayee(nBlockHeight, payee)) {
        // no znode detected...
        // LogPrintf("no znode detected...\n");
        foundMaxVotedPayee = false;
        int nCount = 0;
        CEnode *winningNode = mnodeman.GetNextEnodeInQueueForPayment(nBlockHeight, true, nCount);
        if (!winningNode) {
            // ...and we can't calculate it on our own
            LogPrintf("CEnodePayments::FillBlockPayee -- Failed to detect znode to pay\n");
            return;
        }
        // fill payee with locally calculated winner and hope for the best
        payee = GetScriptForDestination(winningNode->pubKeyCollateralAddress.GetID());
        LogPrintf("payee=%s\n", winningNode->ToString());
    }
    txoutEnodeRet = CTxOut(znodePayment, payee);
    txNew.vout.push_back(txoutEnodeRet);

    CTxDestination address1;
    ExtractDestination(payee, address1);
    CBitcoinAddress address2(address1);
    if (foundMaxVotedPayee) {
        LogPrintf("CEnodePayments::FillBlockPayee::foundMaxVotedPayee -- Enode payment %lld to %s\n", znodePayment, address2.ToString());
    } else {
        LogPrintf("CEnodePayments::FillBlockPayee -- Enode payment %lld to %s\n", znodePayment, address2.ToString());
    }

}

int CEnodePayments::GetMinEnodePaymentsProto() {
    return sporkManager.IsSporkActive(SPORK_10_ENODE_PAY_UPDATED_NODES)
           ? MIN_ENODE_PAYMENT_PROTO_VERSION_2
           : MIN_ENODE_PAYMENT_PROTO_VERSION_1;
}

void CEnodePayments::ProcessMessage(CNode *pfrom, std::string &strCommand, CDataStream &vRecv) {

//    LogPrintf("CEnodePayments::ProcessMessage strCommand=%s\n", strCommand);
    // Ignore any payments messages until enode list is synced
    if (!znodeSync.IsEnodeListSynced()) return;

    if (fLiteMode) return; // disable all Dash specific functionality

    if (strCommand == NetMsgType::ENODEPAYMENTSYNC) { //Enode Payments Request Sync

        // Ignore such requests until we are fully synced.
        // We could start processing this after enode list is synced
        // but this is a heavy one so it's better to finish sync first.
        if (!znodeSync.IsSynced()) return;

        int nCountNeeded;
        vRecv >> nCountNeeded;

        if (netfulfilledman.HasFulfilledRequest(pfrom->addr, NetMsgType::ENODEPAYMENTSYNC)) {
            // Asking for the payments list multiple times in a short period of time is no good
            LogPrintf("ENODEPAYMENTSYNC -- peer already asked me for the list, peer=%d\n", pfrom->id);
            Misbehaving(pfrom->GetId(), 20);
            return;
        }
        netfulfilledman.AddFulfilledRequest(pfrom->addr, NetMsgType::ENODEPAYMENTSYNC);

        Sync(pfrom);
        LogPrint("mnpayments", "ENODEPAYMENTSYNC -- Sent Enode payment votes to peer %d\n", pfrom->id);

    } else if (strCommand == NetMsgType::ENODEPAYMENTVOTE) { // Enode Payments Vote for the Winner

        CEnodePaymentVote vote;
        vRecv >> vote;

        if (pfrom->nVersion < GetMinEnodePaymentsProto()) return;

        if (!pCurrentBlockIndex) return;

        uint256 nHash = vote.GetHash();

        pfrom->setAskFor.erase(nHash);

        {
            LOCK(cs_mapEnodePaymentVotes);
            if (mapEnodePaymentVotes.count(nHash)) {
                LogPrint("mnpayments", "ENODEPAYMENTVOTE -- hash=%s, nHeight=%d seen\n", nHash.ToString(), pCurrentBlockIndex->nHeight);
                return;
            }

            // Avoid processing same vote multiple times
            mapEnodePaymentVotes[nHash] = vote;
            // but first mark vote as non-verified,
            // AddPaymentVote() below should take care of it if vote is actually ok
            mapEnodePaymentVotes[nHash].MarkAsNotVerified();
        }

        int nFirstBlock = pCurrentBlockIndex->nHeight - GetStorageLimit();
        if (vote.nBlockHeight < nFirstBlock || vote.nBlockHeight > pCurrentBlockIndex->nHeight + 20) {
            LogPrint("mnpayments", "ENODEPAYMENTVOTE -- vote out of range: nFirstBlock=%d, nBlockHeight=%d, nHeight=%d\n", nFirstBlock, vote.nBlockHeight, pCurrentBlockIndex->nHeight);
            return;
        }

        std::string strError = "";
        if (!vote.IsValid(pfrom, pCurrentBlockIndex->nHeight, strError)) {
            LogPrint("mnpayments", "ENODEPAYMENTVOTE -- invalid message, error: %s\n", strError);
            return;
        }

        if (!CanVote(vote.vinEnode.prevout, vote.nBlockHeight)) {
            LogPrintf("ENODEPAYMENTVOTE -- znode already voted, znode=%s\n", vote.vinEnode.prevout.ToStringShort());
            return;
        }

        znode_info_t mnInfo = mnodeman.GetEnodeInfo(vote.vinEnode);
        if (!mnInfo.fInfoValid) {
            // mn was not found, so we can't check vote, some info is probably missing
            LogPrintf("ENODEPAYMENTVOTE -- znode is missing %s\n", vote.vinEnode.prevout.ToStringShort());
            mnodeman.AskForMN(pfrom, vote.vinEnode);
            return;
        }

        int nDos = 0;
        if (!vote.CheckSignature(mnInfo.pubKeyEnode, pCurrentBlockIndex->nHeight, nDos)) {
            if (nDos) {
                LogPrintf("ENODEPAYMENTVOTE -- ERROR: invalid signature\n");
                Misbehaving(pfrom->GetId(), nDos);
            } else {
                // only warn about anything non-critical (i.e. nDos == 0) in debug mode
                LogPrint("mnpayments", "ENODEPAYMENTVOTE -- WARNING: invalid signature\n");
            }
            // Either our info or vote info could be outdated.
            // In case our info is outdated, ask for an update,
            mnodeman.AskForMN(pfrom, vote.vinEnode);
            // but there is nothing we can do if vote info itself is outdated
            // (i.e. it was signed by a mn which changed its key),
            // so just quit here.
            return;
        }

        CTxDestination address1;
        ExtractDestination(vote.payee, address1);
        CBitcoinAddress address2(address1);

        LogPrint("mnpayments", "ENODEPAYMENTVOTE -- vote: address=%s, nBlockHeight=%d, nHeight=%d, prevout=%s\n", address2.ToString(), vote.nBlockHeight, pCurrentBlockIndex->nHeight, vote.vinEnode.prevout.ToStringShort());

        if (AddPaymentVote(vote)) {
            vote.Relay();
            znodeSync.AddedPaymentVote();
        }
    }
}

bool CEnodePaymentVote::Sign() {
    std::string strError;
    std::string strMessage = vinEnode.prevout.ToStringShort() +
                             boost::lexical_cast<std::string>(nBlockHeight) +
                             ScriptToAsmStr(payee);

    if (!darkSendSigner.SignMessage(strMessage, vchSig, activeEnode.keyEnode)) {
        LogPrintf("CEnodePaymentVote::Sign -- SignMessage() failed\n");
        return false;
    }

    if (!darkSendSigner.VerifyMessage(activeEnode.pubKeyEnode, vchSig, strMessage, strError)) {
        LogPrintf("CEnodePaymentVote::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CEnodePayments::GetBlockPayee(int nBlockHeight, CScript &payee) {
    if (mapEnodeBlocks.count(nBlockHeight)) {
        return mapEnodeBlocks[nBlockHeight].GetBestPayee(payee);
    }

    return false;
}

// Is this znode scheduled to get paid soon?
// -- Only look ahead up to 8 blocks to allow for propagation of the latest 2 blocks of votes
bool CEnodePayments::IsScheduled(CEnode &mn, int nNotBlockHeight) {
    LOCK(cs_mapEnodeBlocks);

    if (!pCurrentBlockIndex) return false;

    CScript mnpayee;
    mnpayee = GetScriptForDestination(mn.pubKeyCollateralAddress.GetID());

    CScript payee;
    for (int64_t h = pCurrentBlockIndex->nHeight; h <= pCurrentBlockIndex->nHeight + 8; h++) {
        if (h == nNotBlockHeight) continue;
        if (mapEnodeBlocks.count(h) && mapEnodeBlocks[h].GetBestPayee(payee) && mnpayee == payee) {
            return true;
        }
    }

    return false;
}

bool CEnodePayments::AddPaymentVote(const CEnodePaymentVote &vote) {
    LogPrint("znode-payments", "CEnodePayments::AddPaymentVote\n");
    uint256 blockHash = uint256();
    if (!GetBlockHash(blockHash, vote.nBlockHeight - 101)) return false;

    if (HasVerifiedPaymentVote(vote.GetHash())) return false;

    LOCK2(cs_mapEnodeBlocks, cs_mapEnodePaymentVotes);

    mapEnodePaymentVotes[vote.GetHash()] = vote;

    if (!mapEnodeBlocks.count(vote.nBlockHeight)) {
        CEnodeBlockPayees blockPayees(vote.nBlockHeight);
        mapEnodeBlocks[vote.nBlockHeight] = blockPayees;
    }

    mapEnodeBlocks[vote.nBlockHeight].AddPayee(vote);

    return true;
}

bool CEnodePayments::HasVerifiedPaymentVote(uint256 hashIn) {
    LOCK(cs_mapEnodePaymentVotes);
    std::map<uint256, CEnodePaymentVote>::iterator it = mapEnodePaymentVotes.find(hashIn);
    return it != mapEnodePaymentVotes.end() && it->second.IsVerified();
}

void CEnodeBlockPayees::AddPayee(const CEnodePaymentVote &vote) {
    LOCK(cs_vecPayees);

    BOOST_FOREACH(CEnodePayee & payee, vecPayees)
    {
        if (payee.GetPayee() == vote.payee) {
            payee.AddVoteHash(vote.GetHash());
            return;
        }
    }
    CEnodePayee payeeNew(vote.payee, vote.GetHash());
    vecPayees.push_back(payeeNew);
}

bool CEnodeBlockPayees::GetBestPayee(CScript &payeeRet) {
    LOCK(cs_vecPayees);
    LogPrint("mnpayments", "CEnodeBlockPayees::GetBestPayee, vecPayees.size()=%s\n", vecPayees.size());
    if (!vecPayees.size()) {
        LogPrint("mnpayments", "CEnodeBlockPayees::GetBestPayee -- ERROR: couldn't find any payee\n");
        return false;
    }

    int nVotes = -1;
    BOOST_FOREACH(CEnodePayee & payee, vecPayees)
    {
        if (payee.GetVoteCount() > nVotes) {
            payeeRet = payee.GetPayee();
            nVotes = payee.GetVoteCount();
        }
    }

    return (nVotes > -1);
}

bool CEnodeBlockPayees::HasPayeeWithVotes(CScript payeeIn, int nVotesReq) {
    LOCK(cs_vecPayees);

    BOOST_FOREACH(CEnodePayee & payee, vecPayees)
    {
        if (payee.GetVoteCount() >= nVotesReq && payee.GetPayee() == payeeIn) {
            return true;
        }
    }

//    LogPrint("mnpayments", "CEnodeBlockPayees::HasPayeeWithVotes -- ERROR: couldn't find any payee with %d+ votes\n", nVotesReq);
    return false;
}

bool CEnodeBlockPayees::IsTransactionValid(const CTransaction &txNew) {
    LOCK(cs_vecPayees);

    int nMaxSignatures = 0;
    std::string strPayeesPossible = "";

    CAmount nEnodePayment = GetEnodePayment(nBlockHeight, txNew.GetValueOut());

    //require at least MNPAYMENTS_SIGNATURES_REQUIRED signatures

    BOOST_FOREACH(CEnodePayee & payee, vecPayees)
    {
        if (payee.GetVoteCount() >= nMaxSignatures) {
            nMaxSignatures = payee.GetVoteCount();
        }
    }

    // if we don't have at least MNPAYMENTS_SIGNATURES_REQUIRED signatures on a payee, approve whichever is the longest chain
    if (nMaxSignatures < MNPAYMENTS_SIGNATURES_REQUIRED) return true;

    bool hasValidPayee = false;

    BOOST_FOREACH(CEnodePayee & payee, vecPayees)
    {
        if (payee.GetVoteCount() >= MNPAYMENTS_SIGNATURES_REQUIRED) {
            hasValidPayee = true;

            BOOST_FOREACH(CTxOut txout, txNew.vout) {
                if (payee.GetPayee() == txout.scriptPubKey && nEnodePayment == txout.nValue) {
                    LogPrint("mnpayments", "CEnodeBlockPayees::IsTransactionValid -- Found required payment\n");
                    return true;
                }
            }

            CTxDestination address1;
            ExtractDestination(payee.GetPayee(), address1);
            CBitcoinAddress address2(address1);

            if (strPayeesPossible == "") {
                strPayeesPossible = address2.ToString();
            } else {
                strPayeesPossible += "," + address2.ToString();
            }
        }
    }

    if (!hasValidPayee) return true;

    LogPrintf("CEnodeBlockPayees::IsTransactionValid -- ERROR: Missing required payment, possible payees: '%s', amount: %f P2P\n", strPayeesPossible, (float) nEnodePayment / COIN);
    return false;
}

std::string CEnodeBlockPayees::GetRequiredPaymentsString() {
    LOCK(cs_vecPayees);

    std::string strRequiredPayments = "Unknown";

    BOOST_FOREACH(CEnodePayee & payee, vecPayees)
    {
        CTxDestination address1;
        ExtractDestination(payee.GetPayee(), address1);
        CBitcoinAddress address2(address1);

        if (strRequiredPayments != "Unknown") {
            strRequiredPayments += ", " + address2.ToString() + ":" + boost::lexical_cast<std::string>(payee.GetVoteCount());
        } else {
            strRequiredPayments = address2.ToString() + ":" + boost::lexical_cast<std::string>(payee.GetVoteCount());
        }
    }

    return strRequiredPayments;
}

std::string CEnodePayments::GetRequiredPaymentsString(int nBlockHeight) {
    LOCK(cs_mapEnodeBlocks);

    if (mapEnodeBlocks.count(nBlockHeight)) {
        return mapEnodeBlocks[nBlockHeight].GetRequiredPaymentsString();
    }

    return "Unknown";
}

bool CEnodePayments::IsTransactionValid(const CTransaction &txNew, int nBlockHeight) {
    LOCK(cs_mapEnodeBlocks);

    if (mapEnodeBlocks.count(nBlockHeight)) {
        return mapEnodeBlocks[nBlockHeight].IsTransactionValid(txNew);
    }

    return true;
}

void CEnodePayments::CheckAndRemove() {
    if (!pCurrentBlockIndex) return;

    LOCK2(cs_mapEnodeBlocks, cs_mapEnodePaymentVotes);

    int nLimit = GetStorageLimit();

    std::map<uint256, CEnodePaymentVote>::iterator it = mapEnodePaymentVotes.begin();
    while (it != mapEnodePaymentVotes.end()) {
        CEnodePaymentVote vote = (*it).second;

        if (pCurrentBlockIndex->nHeight - vote.nBlockHeight > nLimit) {
            LogPrint("mnpayments", "CEnodePayments::CheckAndRemove -- Removing old Enode payment: nBlockHeight=%d\n", vote.nBlockHeight);
            mapEnodePaymentVotes.erase(it++);
            mapEnodeBlocks.erase(vote.nBlockHeight);
        } else {
            ++it;
        }
    }
    LogPrintf("CEnodePayments::CheckAndRemove -- %s\n", ToString());
}

bool CEnodePaymentVote::IsValid(CNode *pnode, int nValidationHeight, std::string &strError) {
    CEnode *pmn = mnodeman.Find(vinEnode);

    if (!pmn) {
        strError = strprintf("Unknown Enode: prevout=%s", vinEnode.prevout.ToStringShort());
        // Only ask if we are already synced and still have no idea about that Enode
        if (znodeSync.IsEnodeListSynced()) {
            mnodeman.AskForMN(pnode, vinEnode);
        }

        return false;
    }

    int nMinRequiredProtocol;
    if (nBlockHeight >= nValidationHeight) {
        // new votes must comply SPORK_10_ENODE_PAY_UPDATED_NODES rules
        nMinRequiredProtocol = mnpayments.GetMinEnodePaymentsProto();
    } else {
        // allow non-updated znodes for old blocks
        nMinRequiredProtocol = MIN_ENODE_PAYMENT_PROTO_VERSION_1;
    }

    if (pmn->nProtocolVersion < nMinRequiredProtocol) {
        strError = strprintf("Enode protocol is too old: nProtocolVersion=%d, nMinRequiredProtocol=%d", pmn->nProtocolVersion, nMinRequiredProtocol);
        return false;
    }

    // Only znodes should try to check znode rank for old votes - they need to pick the right winner for future blocks.
    // Regular clients (miners included) need to verify znode rank for future block votes only.
    if (!fZNode && nBlockHeight < nValidationHeight) return true;

    int nRank = mnodeman.GetEnodeRank(vinEnode, nBlockHeight - 101, nMinRequiredProtocol, false);

    if (nRank == -1) {
        LogPrint("mnpayments", "CEnodePaymentVote::IsValid -- Can't calculate rank for znode %s\n",
                 vinEnode.prevout.ToStringShort());
        return false;
    }

    if (nRank > MNPAYMENTS_SIGNATURES_TOTAL) {
        // It's common to have znodes mistakenly think they are in the top 10
        // We don't want to print all of these messages in normal mode, debug mode should print though
        strError = strprintf("Enode is not in the top %d (%d)", MNPAYMENTS_SIGNATURES_TOTAL, nRank);
        // Only ban for new mnw which is out of bounds, for old mnw MN list itself might be way too much off
        if (nRank > MNPAYMENTS_SIGNATURES_TOTAL * 2 && nBlockHeight > nValidationHeight) {
            strError = strprintf("Enode is not in the top %d (%d)", MNPAYMENTS_SIGNATURES_TOTAL * 2, nRank);
            LogPrintf("CEnodePaymentVote::IsValid -- Error: %s\n", strError);
            Misbehaving(pnode->GetId(), 20);
        }
        // Still invalid however
        return false;
    }

    return true;
}

bool CEnodePayments::ProcessBlock(int nBlockHeight) {

    // DETERMINE IF WE SHOULD BE VOTING FOR THE NEXT PAYEE

    if (fLiteMode || !fZNode) {
        return false;
    }

    // We have little chances to pick the right winner if winners list is out of sync
    // but we have no choice, so we'll try. However it doesn't make sense to even try to do so
    // if we have not enough data about znodes.
    if (!znodeSync.IsEnodeListSynced()) {
        return false;
    }

    int nRank = mnodeman.GetEnodeRank(activeEnode.vin, nBlockHeight - 101, GetMinEnodePaymentsProto(), false);

    if (nRank == -1) {
        LogPrint("mnpayments", "CEnodePayments::ProcessBlock -- Unknown Enode\n");
        return false;
    }

    if (nRank > MNPAYMENTS_SIGNATURES_TOTAL) {
        LogPrint("mnpayments", "CEnodePayments::ProcessBlock -- Enode not in the top %d (%d)\n", MNPAYMENTS_SIGNATURES_TOTAL, nRank);
        return false;
    }

    // LOCATE THE NEXT ENODE WHICH SHOULD BE PAID

    LogPrintf("CEnodePayments::ProcessBlock -- Start: nBlockHeight=%d, znode=%s\n", nBlockHeight, activeEnode.vin.prevout.ToStringShort());

    // pay to the oldest MN that still had no payment but its input is old enough and it was active long enough
    int nCount = 0;
    CEnode *pmn = mnodeman.GetNextEnodeInQueueForPayment(nBlockHeight, true, nCount);

    if (pmn == NULL) {
        LogPrintf("CEnodePayments::ProcessBlock -- ERROR: Failed to find znode to pay\n");
        return false;
    }

    LogPrintf("CEnodePayments::ProcessBlock -- Enode found by GetNextEnodeInQueueForPayment(): %s\n", pmn->vin.prevout.ToStringShort());


    CScript payee = GetScriptForDestination(pmn->pubKeyCollateralAddress.GetID());

    CEnodePaymentVote voteNew(activeEnode.vin, nBlockHeight, payee);

    CTxDestination address1;
    ExtractDestination(payee, address1);
    CBitcoinAddress address2(address1);

    // SIGN MESSAGE TO NETWORK WITH OUR ENODE KEYS

    if (voteNew.Sign()) {
        if (AddPaymentVote(voteNew)) {
            voteNew.Relay();
            return true;
        }
    }

    return false;
}

void CEnodePaymentVote::Relay() {
    // do not relay until synced
    if (!znodeSync.IsWinnersListSynced()) {
        LogPrintf("CEnodePaymentVote::Relay - znodeSync.IsWinnersListSynced() not sync\n");
        return;
    }
    CInv inv(MSG_ENODE_PAYMENT_VOTE, GetHash());
    RelayInv(inv);
}

bool CEnodePaymentVote::CheckSignature(const CPubKey &pubKeyEnode, int nValidationHeight, int &nDos) {
    // do not ban by default
    nDos = 0;

    std::string strMessage = vinEnode.prevout.ToStringShort() +
                             boost::lexical_cast<std::string>(nBlockHeight) +
                             ScriptToAsmStr(payee);

    std::string strError = "";
    if (!darkSendSigner.VerifyMessage(pubKeyEnode, vchSig, strMessage, strError)) {
        // Only ban for future block vote when we are already synced.
        // Otherwise it could be the case when MN which signed this vote is using another key now
        // and we have no idea about the old one.
        if (znodeSync.IsEnodeListSynced() && nBlockHeight > nValidationHeight) {
            nDos = 20;
        }
        return error("CEnodePaymentVote::CheckSignature -- Got bad Enode payment signature, znode=%s, error: %s", vinEnode.prevout.ToStringShort().c_str(), strError);
    }

    return true;
}

std::string CEnodePaymentVote::ToString() const {
    std::ostringstream info;

    info << vinEnode.prevout.ToStringShort() <<
         ", " << nBlockHeight <<
         ", " << ScriptToAsmStr(payee) <<
         ", " << (int) vchSig.size();

    return info.str();
}

// Send only votes for future blocks, node should request every other missing payment block individually
void CEnodePayments::Sync(CNode *pnode) {
    LOCK(cs_mapEnodeBlocks);

    if (!pCurrentBlockIndex) return;

    int nInvCount = 0;

    for (int h = pCurrentBlockIndex->nHeight; h < pCurrentBlockIndex->nHeight + 20; h++) {
        if (mapEnodeBlocks.count(h)) {
            BOOST_FOREACH(CEnodePayee & payee, mapEnodeBlocks[h].vecPayees)
            {
                std::vector <uint256> vecVoteHashes = payee.GetVoteHashes();
                BOOST_FOREACH(uint256 & hash, vecVoteHashes)
                {
                    if (!HasVerifiedPaymentVote(hash)) continue;
                    pnode->PushInventory(CInv(MSG_ENODE_PAYMENT_VOTE, hash));
                    nInvCount++;
                }
            }
        }
    }

    LogPrintf("CEnodePayments::Sync -- Sent %d votes to peer %d\n", nInvCount, pnode->id);
    pnode->PushMessage(NetMsgType::SYNCSTATUSCOUNT, ENODE_SYNC_MNW, nInvCount);
}

// Request low data/unknown payment blocks in batches directly from some node instead of/after preliminary Sync.
void CEnodePayments::RequestLowDataPaymentBlocks(CNode *pnode) {
    if (!pCurrentBlockIndex) return;

    LOCK2(cs_main, cs_mapEnodeBlocks);

    std::vector <CInv> vToFetch;
    int nLimit = GetStorageLimit();

    const CBlockIndex *pindex = pCurrentBlockIndex;

    while (pCurrentBlockIndex->nHeight - pindex->nHeight < nLimit) {
        if (!mapEnodeBlocks.count(pindex->nHeight)) {
            // We have no idea about this block height, let's ask
            vToFetch.push_back(CInv(MSG_ENODE_PAYMENT_BLOCK, pindex->GetBlockHash()));
            // We should not violate GETDATA rules
            if (vToFetch.size() == MAX_INV_SZ) {
                LogPrintf("CEnodePayments::SyncLowDataPaymentBlocks -- asking peer %d for %d blocks\n", pnode->id, MAX_INV_SZ);
                pnode->PushMessage(NetMsgType::GETDATA, vToFetch);
                // Start filling new batch
                vToFetch.clear();
            }
        }
        if (!pindex->pprev) break;
        pindex = pindex->pprev;
    }

    std::map<int, CEnodeBlockPayees>::iterator it = mapEnodeBlocks.begin();

    while (it != mapEnodeBlocks.end()) {
        int nTotalVotes = 0;
        bool fFound = false;
        BOOST_FOREACH(CEnodePayee & payee, it->second.vecPayees)
        {
            if (payee.GetVoteCount() >= MNPAYMENTS_SIGNATURES_REQUIRED) {
                fFound = true;
                break;
            }
            nTotalVotes += payee.GetVoteCount();
        }
        // A clear winner (MNPAYMENTS_SIGNATURES_REQUIRED+ votes) was found
        // or no clear winner was found but there are at least avg number of votes
        if (fFound || nTotalVotes >= (MNPAYMENTS_SIGNATURES_TOTAL + MNPAYMENTS_SIGNATURES_REQUIRED) / 2) {
            // so just move to the next block
            ++it;
            continue;
        }
        // DEBUG
//        DBG (
//            // Let's see why this failed
//            BOOST_FOREACH(CEnodePayee& payee, it->second.vecPayees) {
//                CTxDestination address1;
//                ExtractDestination(payee.GetPayee(), address1);
//                CBitcoinAddress address2(address1);
//                printf("payee %s votes %d\n", address2.ToString().c_str(), payee.GetVoteCount());
//            }
//            printf("block %d votes total %d\n", it->first, nTotalVotes);
//        )
        // END DEBUG
        // Low data block found, let's try to sync it
        uint256 hash;
        if (GetBlockHash(hash, it->first)) {
            vToFetch.push_back(CInv(MSG_ENODE_PAYMENT_BLOCK, hash));
        }
        // We should not violate GETDATA rules
        if (vToFetch.size() == MAX_INV_SZ) {
            LogPrintf("CEnodePayments::SyncLowDataPaymentBlocks -- asking peer %d for %d payment blocks\n", pnode->id, MAX_INV_SZ);
            pnode->PushMessage(NetMsgType::GETDATA, vToFetch);
            // Start filling new batch
            vToFetch.clear();
        }
        ++it;
    }
    // Ask for the rest of it
    if (!vToFetch.empty()) {
        LogPrintf("CEnodePayments::SyncLowDataPaymentBlocks -- asking peer %d for %d payment blocks\n", pnode->id, vToFetch.size());
        pnode->PushMessage(NetMsgType::GETDATA, vToFetch);
    }
}

std::string CEnodePayments::ToString() const {
    std::ostringstream info;

    info << "Votes: " << (int) mapEnodePaymentVotes.size() <<
         ", Blocks: " << (int) mapEnodeBlocks.size();

    return info.str();
}

bool CEnodePayments::IsEnoughData() {
    float nAverageVotes = (MNPAYMENTS_SIGNATURES_TOTAL + MNPAYMENTS_SIGNATURES_REQUIRED) / 2;
    int nStorageLimit = GetStorageLimit();
    return GetBlockCount() > nStorageLimit && GetVoteCount() > nStorageLimit * nAverageVotes;
}

int CEnodePayments::GetStorageLimit() {
    return std::max(int(mnodeman.size() * nStorageCoeff), nMinBlocksToStore);
}

void CEnodePayments::UpdatedBlockTip(const CBlockIndex *pindex) {
    pCurrentBlockIndex = pindex;
    LogPrint("mnpayments", "CEnodePayments::UpdatedBlockTip -- pCurrentBlockIndex->nHeight=%d\n", pCurrentBlockIndex->nHeight);
    
    ProcessBlock(pindex->nHeight + 5);
}
