// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activeznode.h"
#include "consensus/validation.h"
#include "darksend.h"
#include "init.h"
//#include "governance.h"
#include "znode.h"
#include "znode-payments.h"
#include "znode-sync.h"
#include "znodeman.h"
#include "util.h"

#include <boost/lexical_cast.hpp>


CEnode::CEnode() :
        vin(),
        addr(),
        pubKeyCollateralAddress(),
        pubKeyEnode(),
        lastPing(),
        vchSig(),
        sigTime(GetAdjustedTime()),
        nLastDsq(0),
        nTimeLastChecked(0),
        nTimeLastPaid(0),
        nTimeLastWatchdogVote(0),
        nActiveState(ENODE_ENABLED),
        nCacheCollateralBlock(0),
        nBlockLastPaid(0),
        nProtocolVersion(PROTOCOL_VERSION),
        nPoSeBanScore(0),
        nPoSeBanHeight(0),
        fAllowMixingTx(true),
        fUnitTest(false) {}

CEnode::CEnode(CService addrNew, CTxIn vinNew, CPubKey pubKeyCollateralAddressNew, CPubKey pubKeyEnodeNew, int nProtocolVersionIn) :
        vin(vinNew),
        addr(addrNew),
        pubKeyCollateralAddress(pubKeyCollateralAddressNew),
        pubKeyEnode(pubKeyEnodeNew),
        lastPing(),
        vchSig(),
        sigTime(GetAdjustedTime()),
        nLastDsq(0),
        nTimeLastChecked(0),
        nTimeLastPaid(0),
        nTimeLastWatchdogVote(0),
        nActiveState(ENODE_ENABLED),
        nCacheCollateralBlock(0),
        nBlockLastPaid(0),
        nProtocolVersion(nProtocolVersionIn),
        nPoSeBanScore(0),
        nPoSeBanHeight(0),
        fAllowMixingTx(true),
        fUnitTest(false) {}

CEnode::CEnode(const CEnode &other) :
        vin(other.vin),
        addr(other.addr),
        pubKeyCollateralAddress(other.pubKeyCollateralAddress),
        pubKeyEnode(other.pubKeyEnode),
        lastPing(other.lastPing),
        vchSig(other.vchSig),
        sigTime(other.sigTime),
        nLastDsq(other.nLastDsq),
        nTimeLastChecked(other.nTimeLastChecked),
        nTimeLastPaid(other.nTimeLastPaid),
        nTimeLastWatchdogVote(other.nTimeLastWatchdogVote),
        nActiveState(other.nActiveState),
        nCacheCollateralBlock(other.nCacheCollateralBlock),
        nBlockLastPaid(other.nBlockLastPaid),
        nProtocolVersion(other.nProtocolVersion),
        nPoSeBanScore(other.nPoSeBanScore),
        nPoSeBanHeight(other.nPoSeBanHeight),
        fAllowMixingTx(other.fAllowMixingTx),
        fUnitTest(other.fUnitTest) {}

CEnode::CEnode(const CEnodeBroadcast &mnb) :
        vin(mnb.vin),
        addr(mnb.addr),
        pubKeyCollateralAddress(mnb.pubKeyCollateralAddress),
        pubKeyEnode(mnb.pubKeyEnode),
        lastPing(mnb.lastPing),
        vchSig(mnb.vchSig),
        sigTime(mnb.sigTime),
        nLastDsq(0),
        nTimeLastChecked(0),
        nTimeLastPaid(0),
        nTimeLastWatchdogVote(mnb.sigTime),
        nActiveState(mnb.nActiveState),
        nCacheCollateralBlock(0),
        nBlockLastPaid(0),
        nProtocolVersion(mnb.nProtocolVersion),
        nPoSeBanScore(0),
        nPoSeBanHeight(0),
        fAllowMixingTx(true),
        fUnitTest(false) {}

//CSporkManager sporkManager;
//
// When a new znode broadcast is sent, update our information
//
bool CEnode::UpdateFromNewBroadcast(CEnodeBroadcast &mnb) {
    if (mnb.sigTime <= sigTime && !mnb.fRecovery) return false;

    pubKeyEnode = mnb.pubKeyEnode;
    sigTime = mnb.sigTime;
    vchSig = mnb.vchSig;
    nProtocolVersion = mnb.nProtocolVersion;
    addr = mnb.addr;
    nPoSeBanScore = 0;
    nPoSeBanHeight = 0;
    nTimeLastChecked = 0;
    int nDos = 0;
    if (mnb.lastPing == CEnodePing() || (mnb.lastPing != CEnodePing() && mnb.lastPing.CheckAndUpdate(this, true, nDos))) {
        lastPing = mnb.lastPing;
        mnodeman.mapSeenEnodePing.insert(std::make_pair(lastPing.GetHash(), lastPing));
    }
    // if it matches our Enode privkey...
    if (fZNode && pubKeyEnode == activeEnode.pubKeyEnode) {
        nPoSeBanScore = -ENODE_POSE_BAN_MAX_SCORE;
        if (nProtocolVersion == PROTOCOL_VERSION) {
            // ... and PROTOCOL_VERSION, then we've been remotely activated ...
            activeEnode.ManageState();
        } else {
            // ... otherwise we need to reactivate our node, do not add it to the list and do not relay
            // but also do not ban the node we get this message from
            LogPrintf("CEnode::UpdateFromNewBroadcast -- wrong PROTOCOL_VERSION, re-activate your MN: message nProtocolVersion=%d  PROTOCOL_VERSION=%d\n", nProtocolVersion, PROTOCOL_VERSION);
            return false;
        }
    }
    return true;
}

//
// Deterministically calculate a given "score" for a Enode depending on how close it's hash is to
// the proof of work for that block. The further away they are the better, the furthest will win the election
// and get paid this block
//
arith_uint256 CEnode::CalculateScore(const uint256 &blockHash) {
    uint256 aux = ArithToUint256(UintToArith256(vin.prevout.hash) + vin.prevout.n);

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << blockHash;
    arith_uint256 hash2 = UintToArith256(ss.GetHash());

    CHashWriter ss2(SER_GETHASH, PROTOCOL_VERSION);
    ss2 << blockHash;
    ss2 << aux;
    arith_uint256 hash3 = UintToArith256(ss2.GetHash());

    return (hash3 > hash2 ? hash3 - hash2 : hash2 - hash3);
}

void CEnode::Check(bool fForce) {
    LOCK(cs);

    if (ShutdownRequested()) return;

    if (!fForce && (GetTime() - nTimeLastChecked < ENODE_CHECK_SECONDS)) return;
    nTimeLastChecked = GetTime();

    LogPrint("znode", "CEnode::Check -- Enode %s is in %s state\n", vin.prevout.ToStringShort(), GetStateString());

    //once spent, stop doing the checks
    if (IsOutpointSpent()) return;

    int nHeight = 0;
    if (!fUnitTest) {
        TRY_LOCK(cs_main, lockMain);
        if (!lockMain) return;

        CCoins coins;
        if (!pcoinsTip->GetCoins(vin.prevout.hash, coins) ||
            (unsigned int) vin.prevout.n >= coins.vout.size() ||
            coins.vout[vin.prevout.n].IsNull()) {
            nActiveState = ENODE_OUTPOINT_SPENT;
            LogPrint("znode", "CEnode::Check -- Failed to find Enode UTXO, znode=%s\n", vin.prevout.ToStringShort());
            return;
        }

        nHeight = chainActive.Height();
    }

    if (IsPoSeBanned()) {
        if (nHeight < nPoSeBanHeight) return; // too early?
        // Otherwise give it a chance to proceed further to do all the usual checks and to change its state.
        // Enode still will be on the edge and can be banned back easily if it keeps ignoring mnverify
        // or connect attempts. Will require few mnverify messages to strengthen its position in mn list.
        LogPrintf("CEnode::Check -- Enode %s is unbanned and back in list now\n", vin.prevout.ToStringShort());
        DecreasePoSeBanScore();
    } else if (nPoSeBanScore >= ENODE_POSE_BAN_MAX_SCORE) {
        nActiveState = ENODE_POSE_BAN;
        // ban for the whole payment cycle
        nPoSeBanHeight = nHeight + mnodeman.size();
        LogPrintf("CEnode::Check -- Enode %s is banned till block %d now\n", vin.prevout.ToStringShort(), nPoSeBanHeight);
        return;
    }

    int nActiveStatePrev = nActiveState;
    bool fOurEnode = fZNode && activeEnode.pubKeyEnode == pubKeyEnode;

    // znode doesn't meet payment protocol requirements ...
    bool fRequireUpdate = nProtocolVersion < mnpayments.GetMinEnodePaymentsProto() ||
                          // or it's our own node and we just updated it to the new protocol but we are still waiting for activation ...
                          (fOurEnode && nProtocolVersion < PROTOCOL_VERSION);

    if (fRequireUpdate) {
        nActiveState = ENODE_UPDATE_REQUIRED;
        if (nActiveStatePrev != nActiveState) {
            LogPrint("znode", "CEnode::Check -- Enode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
        }
        return;
    }

    // keep old znodes on start, give them a chance to receive updates...
    bool fWaitForPing = !znodeSync.IsEnodeListSynced() && !IsPingedWithin(ENODE_MIN_MNP_SECONDS);

    if (fWaitForPing && !fOurEnode) {
        // ...but if it was already expired before the initial check - return right away
        if (IsExpired() || IsWatchdogExpired() || IsNewStartRequired()) {
            LogPrint("znode", "CEnode::Check -- Enode %s is in %s state, waiting for ping\n", vin.prevout.ToStringShort(), GetStateString());
            return;
        }
    }

    // don't expire if we are still in "waiting for ping" mode unless it's our own znode
    if (!fWaitForPing || fOurEnode) {

        if (!IsPingedWithin(ENODE_NEW_START_REQUIRED_SECONDS)) {
            nActiveState = ENODE_NEW_START_REQUIRED;
            if (nActiveStatePrev != nActiveState) {
                LogPrint("znode", "CEnode::Check -- Enode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
            }
            return;
        }

        bool fWatchdogActive = znodeSync.IsSynced() && mnodeman.IsWatchdogActive();
        bool fWatchdogExpired = (fWatchdogActive && ((GetTime() - nTimeLastWatchdogVote) > ENODE_WATCHDOG_MAX_SECONDS));

//        LogPrint("znode", "CEnode::Check -- outpoint=%s, nTimeLastWatchdogVote=%d, GetTime()=%d, fWatchdogExpired=%d\n",
//                vin.prevout.ToStringShort(), nTimeLastWatchdogVote, GetTime(), fWatchdogExpired);

        if (fWatchdogExpired) {
            nActiveState = ENODE_WATCHDOG_EXPIRED;
            if (nActiveStatePrev != nActiveState) {
                LogPrint("znode", "CEnode::Check -- Enode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
            }
            return;
        }

        if (!IsPingedWithin(ENODE_EXPIRATION_SECONDS)) {
            nActiveState = ENODE_EXPIRED;
            if (nActiveStatePrev != nActiveState) {
                LogPrint("znode", "CEnode::Check -- Enode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
            }
            return;
        }
    }

    if (lastPing.sigTime - sigTime < ENODE_MIN_MNP_SECONDS) {
        nActiveState = ENODE_PRE_ENABLED;
        if (nActiveStatePrev != nActiveState) {
            LogPrint("znode", "CEnode::Check -- Enode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
        }
        return;
    }

    nActiveState = ENODE_ENABLED; // OK
    if (nActiveStatePrev != nActiveState) {
        LogPrint("znode", "CEnode::Check -- Enode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
    }
}

bool CEnode::IsValidNetAddr() {
    return IsValidNetAddr(addr);
}

bool CEnode::IsValidForPayment() {
    if (nActiveState == ENODE_ENABLED) {
        return true;
    }
//    if(!sporkManager.IsSporkActive(SPORK_14_REQUIRE_SENTINEL_FLAG) &&
//       (nActiveState == ENODE_WATCHDOG_EXPIRED)) {
//        return true;
//    }

    return false;
}

bool CEnode::IsValidNetAddr(CService addrIn) {
    // TODO: regtest is fine with any addresses for now,
    // should probably be a bit smarter if one day we start to implement tests for this
    return Params().NetworkIDString() == CBaseChainParams::REGTEST ||
           (addrIn.IsIPv4() && IsReachable(addrIn) && addrIn.IsRoutable());
}

znode_info_t CEnode::GetInfo() {
    znode_info_t info;
    info.vin = vin;
    info.addr = addr;
    info.pubKeyCollateralAddress = pubKeyCollateralAddress;
    info.pubKeyEnode = pubKeyEnode;
    info.sigTime = sigTime;
    info.nLastDsq = nLastDsq;
    info.nTimeLastChecked = nTimeLastChecked;
    info.nTimeLastPaid = nTimeLastPaid;
    info.nTimeLastWatchdogVote = nTimeLastWatchdogVote;
    info.nTimeLastPing = lastPing.sigTime;
    info.nActiveState = nActiveState;
    info.nProtocolVersion = nProtocolVersion;
    info.fInfoValid = true;
    return info;
}

std::string CEnode::StateToString(int nStateIn) {
    switch (nStateIn) {
        case ENODE_PRE_ENABLED:
            return "PRE_ENABLED";
        case ENODE_ENABLED:
            return "ENABLED";
        case ENODE_EXPIRED:
            return "EXPIRED";
        case ENODE_OUTPOINT_SPENT:
            return "OUTPOINT_SPENT";
        case ENODE_UPDATE_REQUIRED:
            return "UPDATE_REQUIRED";
        case ENODE_WATCHDOG_EXPIRED:
            return "WATCHDOG_EXPIRED";
        case ENODE_NEW_START_REQUIRED:
            return "NEW_START_REQUIRED";
        case ENODE_POSE_BAN:
            return "POSE_BAN";
        default:
            return "UNKNOWN";
    }
}

std::string CEnode::GetStateString() const {
    return StateToString(nActiveState);
}

std::string CEnode::GetStatus() const {
    // TODO: return smth a bit more human readable here
    return GetStateString();
}

std::string CEnode::ToString() const {
    std::string str;
    str += "znode{";
    str += addr.ToString();
    str += " ";
    str += std::to_string(nProtocolVersion);
    str += " ";
    str += vin.prevout.ToStringShort();
    str += " ";
    str += CBitcoinAddress(pubKeyCollateralAddress.GetID()).ToString();
    str += " ";
    str += std::to_string(lastPing == CEnodePing() ? sigTime : lastPing.sigTime);
    str += " ";
    str += std::to_string(lastPing == CEnodePing() ? 0 : lastPing.sigTime - sigTime);
    str += " ";
    str += std::to_string(nBlockLastPaid);
    str += "}\n";
    return str;
}

int CEnode::GetCollateralAge() {
    int nHeight;
    {
        TRY_LOCK(cs_main, lockMain);
        if (!lockMain || !chainActive.Tip()) return -1;
        nHeight = chainActive.Height();
    }

    if (nCacheCollateralBlock == 0) {
        int nInputAge = GetInputAge(vin);
        if (nInputAge > 0) {
            nCacheCollateralBlock = nHeight - nInputAge;
        } else {
            return nInputAge;
        }
    }

    return nHeight - nCacheCollateralBlock;
}

void CEnode::UpdateLastPaid(const CBlockIndex *pindex, int nMaxBlocksToScanBack) {
    if (!pindex) {
        LogPrintf("CEnode::UpdateLastPaid pindex is NULL\n");
        return;
    }

    const CBlockIndex *BlockReading = pindex;

    CScript mnpayee = GetScriptForDestination(pubKeyCollateralAddress.GetID());
    LogPrint("znode", "CEnode::UpdateLastPaidBlock -- searching for block with payment to %s\n", vin.prevout.ToStringShort());

    LOCK(cs_mapEnodeBlocks);

    for (int i = 0; BlockReading && BlockReading->nHeight > nBlockLastPaid && i < nMaxBlocksToScanBack; i++) {
//        LogPrintf("mnpayments.mapEnodeBlocks.count(BlockReading->nHeight)=%s\n", mnpayments.mapEnodeBlocks.count(BlockReading->nHeight));
//        LogPrintf("mnpayments.mapEnodeBlocks[BlockReading->nHeight].HasPayeeWithVotes(mnpayee, 2)=%s\n", mnpayments.mapEnodeBlocks[BlockReading->nHeight].HasPayeeWithVotes(mnpayee, 2));
        if (mnpayments.mapEnodeBlocks.count(BlockReading->nHeight) &&
            mnpayments.mapEnodeBlocks[BlockReading->nHeight].HasPayeeWithVotes(mnpayee, 2)) {
            // LogPrintf("i=%s, BlockReading->nHeight=%s\n", i, BlockReading->nHeight);
            CBlock block;
            if (!ReadBlockFromDisk(block, BlockReading, Params().GetConsensus())) // shouldn't really happen
            {
                LogPrintf("ReadBlockFromDisk failed\n");
                continue;
            }

            CAmount nEnodePayment = GetEnodePayment(BlockReading->nHeight, block.vtx[0].GetValueOut());

            BOOST_FOREACH(CTxOut txout, block.vtx[0].vout)
            if (mnpayee == txout.scriptPubKey && nEnodePayment == txout.nValue) {
                nBlockLastPaid = BlockReading->nHeight;
                nTimeLastPaid = BlockReading->nTime;
                LogPrint("znode", "CEnode::UpdateLastPaidBlock -- searching for block with payment to %s -- found new %d\n", vin.prevout.ToStringShort(), nBlockLastPaid);
                return;
            }
        }

        if (BlockReading->pprev == NULL) {
            assert(BlockReading);
            break;
        }
        BlockReading = BlockReading->pprev;
    }

    // Last payment for this znode wasn't found in latest mnpayments blocks
    // or it was found in mnpayments blocks but wasn't found in the blockchain.
    // LogPrint("znode", "CEnode::UpdateLastPaidBlock -- searching for block with payment to %s -- keeping old %d\n", vin.prevout.ToStringShort(), nBlockLastPaid);
}

bool CEnodeBroadcast::Create(std::string strService, std::string strKeyEnode, std::string strTxHash, std::string strOutputIndex, std::string &strErrorRet, CEnodeBroadcast &mnbRet, bool fOffline) {
    LogPrintf("CEnodeBroadcast::Create\n");
    CTxIn txin;
    CPubKey pubKeyCollateralAddressNew;
    CKey keyCollateralAddressNew;
    CPubKey pubKeyEnodeNew;
    CKey keyEnodeNew;
    //need correct blocks to send ping
    if (!fOffline && !znodeSync.IsBlockchainSynced()) {
        strErrorRet = "Sync in progress. Must wait until sync is complete to start Enode";
        LogPrintf("CEnodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    //TODO
    if (!darkSendSigner.GetKeysFromSecret(strKeyEnode, keyEnodeNew, pubKeyEnodeNew)) {
        strErrorRet = strprintf("Invalid znode key %s", strKeyEnode);
        LogPrintf("CEnodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    if (!pwalletMain->GetEnodeVinAndKeys(txin, pubKeyCollateralAddressNew, keyCollateralAddressNew, strTxHash, strOutputIndex)) {
        strErrorRet = strprintf("Could not allocate txin %s:%s for znode %s", strTxHash, strOutputIndex, strService);
        LogPrintf("CEnodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    CService service = CService(strService);
    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (service.GetPort() != mainnetDefaultPort) {
            strErrorRet = strprintf("Invalid port %u for znode %s, only %d is supported on mainnet.", service.GetPort(), strService, mainnetDefaultPort);
            LogPrintf("CEnodeBroadcast::Create -- %s\n", strErrorRet);
            return false;
        }
    } else if (service.GetPort() == mainnetDefaultPort) {
        strErrorRet = strprintf("Invalid port %u for znode %s, %d is the only supported on mainnet.", service.GetPort(), strService, mainnetDefaultPort);
        LogPrintf("CEnodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    return Create(txin, CService(strService), keyCollateralAddressNew, pubKeyCollateralAddressNew, keyEnodeNew, pubKeyEnodeNew, strErrorRet, mnbRet);
}

bool CEnodeBroadcast::Create(CTxIn txin, CService service, CKey keyCollateralAddressNew, CPubKey pubKeyCollateralAddressNew, CKey keyEnodeNew, CPubKey pubKeyEnodeNew, std::string &strErrorRet, CEnodeBroadcast &mnbRet) {
    // wait for reindex and/or import to finish
    if (fImporting || fReindex) return false;

    LogPrint("znode", "CEnodeBroadcast::Create -- pubKeyCollateralAddressNew = %s, pubKeyEnodeNew.GetID() = %s\n",
             CBitcoinAddress(pubKeyCollateralAddressNew.GetID()).ToString(),
             pubKeyEnodeNew.GetID().ToString());


    CEnodePing mnp(txin);
    if (!mnp.Sign(keyEnodeNew, pubKeyEnodeNew)) {
        strErrorRet = strprintf("Failed to sign ping, znode=%s", txin.prevout.ToStringShort());
        LogPrintf("CEnodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CEnodeBroadcast();
        return false;
    }

    mnbRet = CEnodeBroadcast(service, txin, pubKeyCollateralAddressNew, pubKeyEnodeNew, PROTOCOL_VERSION);

    if (!mnbRet.IsValidNetAddr()) {
        strErrorRet = strprintf("Invalid IP address, znode=%s", txin.prevout.ToStringShort());
        LogPrintf("CEnodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CEnodeBroadcast();
        return false;
    }

    mnbRet.lastPing = mnp;
    if (!mnbRet.Sign(keyCollateralAddressNew)) {
        strErrorRet = strprintf("Failed to sign broadcast, znode=%s", txin.prevout.ToStringShort());
        LogPrintf("CEnodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CEnodeBroadcast();
        return false;
    }

    return true;
}

bool CEnodeBroadcast::SimpleCheck(int &nDos) {
    nDos = 0;

    // make sure addr is valid
    if (!IsValidNetAddr()) {
        LogPrintf("CEnodeBroadcast::SimpleCheck -- Invalid addr, rejected: znode=%s  addr=%s\n",
                  vin.prevout.ToStringShort(), addr.ToString());
        return false;
    }

    // make sure signature isn't in the future (past is OK)
    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrintf("CEnodeBroadcast::SimpleCheck -- Signature rejected, too far into the future: znode=%s\n", vin.prevout.ToStringShort());
        nDos = 1;
        return false;
    }

    // empty ping or incorrect sigTime/unknown blockhash
    if (lastPing == CEnodePing() || !lastPing.SimpleCheck(nDos)) {
        // one of us is probably forked or smth, just mark it as expired and check the rest of the rules
        nActiveState = ENODE_EXPIRED;
    }

    if (nProtocolVersion < mnpayments.GetMinEnodePaymentsProto()) {
        LogPrintf("CEnodeBroadcast::SimpleCheck -- ignoring outdated Enode: znode=%s  nProtocolVersion=%d\n", vin.prevout.ToStringShort(), nProtocolVersion);
        return false;
    }

    CScript pubkeyScript;
    pubkeyScript = GetScriptForDestination(pubKeyCollateralAddress.GetID());

    if (pubkeyScript.size() != 25) {
        LogPrintf("CEnodeBroadcast::SimpleCheck -- pubKeyCollateralAddress has the wrong size\n");
        nDos = 100;
        return false;
    }

    CScript pubkeyScript2;
    pubkeyScript2 = GetScriptForDestination(pubKeyEnode.GetID());

    if (pubkeyScript2.size() != 25) {
        LogPrintf("CEnodeBroadcast::SimpleCheck -- pubKeyEnode has the wrong size\n");
        nDos = 100;
        return false;
    }

    if (!vin.scriptSig.empty()) {
        LogPrintf("CEnodeBroadcast::SimpleCheck -- Ignore Not Empty ScriptSig %s\n", vin.ToString());
        nDos = 100;
        return false;
    }

    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (addr.GetPort() != mainnetDefaultPort) return false;
    } else if (addr.GetPort() == mainnetDefaultPort) return false;

    return true;
}

bool CEnodeBroadcast::Update(CEnode *pmn, int &nDos) {
    nDos = 0;

    if (pmn->sigTime == sigTime && !fRecovery) {
        // mapSeenEnodeBroadcast in CEnodeMan::CheckMnbAndUpdateEnodeList should filter legit duplicates
        // but this still can happen if we just started, which is ok, just do nothing here.
        return false;
    }

    // this broadcast is older than the one that we already have - it's bad and should never happen
    // unless someone is doing something fishy
    if (pmn->sigTime > sigTime) {
        LogPrintf("CEnodeBroadcast::Update -- Bad sigTime %d (existing broadcast is at %d) for Enode %s %s\n",
                  sigTime, pmn->sigTime, vin.prevout.ToStringShort(), addr.ToString());
        return false;
    }

    pmn->Check();

    // znode is banned by PoSe
    if (pmn->IsPoSeBanned()) {
        LogPrintf("CEnodeBroadcast::Update -- Banned by PoSe, znode=%s\n", vin.prevout.ToStringShort());
        return false;
    }

    // IsVnAssociatedWithPubkey is validated once in CheckOutpoint, after that they just need to match
    if (pmn->pubKeyCollateralAddress != pubKeyCollateralAddress) {
        LogPrintf("CEnodeBroadcast::Update -- Got mismatched pubKeyCollateralAddress and vin\n");
        nDos = 33;
        return false;
    }

    if (!CheckSignature(nDos)) {
        LogPrintf("CEnodeBroadcast::Update -- CheckSignature() failed, znode=%s\n", vin.prevout.ToStringShort());
        return false;
    }

    // if ther was no znode broadcast recently or if it matches our Enode privkey...
    if (!pmn->IsBroadcastedWithin(ENODE_MIN_MNB_SECONDS) || (fZNode && pubKeyEnode == activeEnode.pubKeyEnode)) {
        // take the newest entry
        LogPrintf("CEnodeBroadcast::Update -- Got UPDATED Enode entry: addr=%s\n", addr.ToString());
        if (pmn->UpdateFromNewBroadcast((*this))) {
            pmn->Check();
            RelayZNode();
        }
        znodeSync.AddedEnodeList();
    }

    return true;
}

bool CEnodeBroadcast::CheckOutpoint(int &nDos) {
    // we are a znode with the same vin (i.e. already activated) and this mnb is ours (matches our Enode privkey)
    // so nothing to do here for us
    if (fZNode && vin.prevout == activeEnode.vin.prevout && pubKeyEnode == activeEnode.pubKeyEnode) {
        return false;
    }

    if (!CheckSignature(nDos)) {
        LogPrintf("CEnodeBroadcast::CheckOutpoint -- CheckSignature() failed, znode=%s\n", vin.prevout.ToStringShort());
        return false;
    }

    {
        TRY_LOCK(cs_main, lockMain);
        if (!lockMain) {
            // not mnb fault, let it to be checked again later
            LogPrint("znode", "CEnodeBroadcast::CheckOutpoint -- Failed to aquire lock, addr=%s", addr.ToString());
            mnodeman.mapSeenEnodeBroadcast.erase(GetHash());
            return false;
        }

        CCoins coins;
        if (!pcoinsTip->GetCoins(vin.prevout.hash, coins) ||
            (unsigned int) vin.prevout.n >= coins.vout.size() ||
            coins.vout[vin.prevout.n].IsNull()) {
            LogPrint("znode", "CEnodeBroadcast::CheckOutpoint -- Failed to find Enode UTXO, znode=%s\n", vin.prevout.ToStringShort());
            return false;
        }
        if (coins.vout[vin.prevout.n].nValue != ENODE_COIN_REQUIRED * COIN) {
            LogPrint("znode", "CEnodeBroadcast::CheckOutpoint -- Enode UTXO should have 1000 P2P, znode=%s\n", vin.prevout.ToStringShort());
            return false;
        }
        if (chainActive.Height() - coins.nHeight + 1 < Params().GetConsensus().nEnodeMinimumConfirmations) {
            LogPrintf("CEnodeBroadcast::CheckOutpoint -- Enode UTXO must have at least %d confirmations, znode=%s\n",
                      Params().GetConsensus().nEnodeMinimumConfirmations, vin.prevout.ToStringShort());
            // maybe we miss few blocks, let this mnb to be checked again later
            mnodeman.mapSeenEnodeBroadcast.erase(GetHash());
            return false;
        }
    }

    LogPrint("znode", "CEnodeBroadcast::CheckOutpoint -- Enode UTXO verified\n");

    // make sure the vout that was signed is related to the transaction that spawned the Enode
    //  - this is expensive, so it's only done once per Enode
    if (!darkSendSigner.IsVinAssociatedWithPubkey(vin, pubKeyCollateralAddress)) {
        LogPrintf("CEnodeMan::CheckOutpoint -- Got mismatched pubKeyCollateralAddress and vin\n");
        nDos = 33;
        return false;
    }

    // verify that sig time is legit in past
    // should be at least not earlier than block when 1000 P2P tx got nEnodeMinimumConfirmations
    uint256 hashBlock = uint256();
    CTransaction tx2;
    GetTransaction(vin.prevout.hash, tx2, Params().GetConsensus(), hashBlock, true);
    {
        LOCK(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
        if (mi != mapBlockIndex.end() && (*mi).second) {
            CBlockIndex *pMNIndex = (*mi).second; // block for 1000 P2P tx -> 1 confirmation
            CBlockIndex *pConfIndex = chainActive[pMNIndex->nHeight + Params().GetConsensus().nEnodeMinimumConfirmations - 1]; // block where tx got nEnodeMinimumConfirmations
            if (pConfIndex->GetBlockTime() > sigTime) {
                LogPrintf("CEnodeBroadcast::CheckOutpoint -- Bad sigTime %d (%d conf block is at %d) for Enode %s %s\n",
                          sigTime, Params().GetConsensus().nEnodeMinimumConfirmations, pConfIndex->GetBlockTime(), vin.prevout.ToStringShort(), addr.ToString());
                return false;
            }
        }
    }

    return true;
}

bool CEnodeBroadcast::Sign(CKey &keyCollateralAddress) {
    std::string strError;
    std::string strMessage;

    sigTime = GetAdjustedTime();

    strMessage = addr.ToString() + boost::lexical_cast<std::string>(sigTime) +
                 pubKeyCollateralAddress.GetID().ToString() + pubKeyEnode.GetID().ToString() +
                 boost::lexical_cast<std::string>(nProtocolVersion);

    if (!darkSendSigner.SignMessage(strMessage, vchSig, keyCollateralAddress)) {
        LogPrintf("CEnodeBroadcast::Sign -- SignMessage() failed\n");
        return false;
    }

    if (!darkSendSigner.VerifyMessage(pubKeyCollateralAddress, vchSig, strMessage, strError)) {
        LogPrintf("CEnodeBroadcast::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CEnodeBroadcast::CheckSignature(int &nDos) {
    std::string strMessage;
    std::string strError = "";
    nDos = 0;

    strMessage = addr.ToString() + boost::lexical_cast<std::string>(sigTime) +
                 pubKeyCollateralAddress.GetID().ToString() + pubKeyEnode.GetID().ToString() +
                 boost::lexical_cast<std::string>(nProtocolVersion);

    LogPrint("znode", "CEnodeBroadcast::CheckSignature -- strMessage: %s  pubKeyCollateralAddress address: %s  sig: %s\n", strMessage, CBitcoinAddress(pubKeyCollateralAddress.GetID()).ToString(), EncodeBase64(&vchSig[0], vchSig.size()));

    if (!darkSendSigner.VerifyMessage(pubKeyCollateralAddress, vchSig, strMessage, strError)) {
        LogPrintf("CEnodeBroadcast::CheckSignature -- Got bad Enode announce signature, error: %s\n", strError);
        nDos = 100;
        return false;
    }

    return true;
}

void CEnodeBroadcast::RelayZNode() {
    LogPrintf("CEnodeBroadcast::RelayZNode\n");
    CInv inv(MSG_ENODE_ANNOUNCE, GetHash());
    RelayInv(inv);
}

CEnodePing::CEnodePing(CTxIn &vinNew) {
    LOCK(cs_main);
    if (!chainActive.Tip() || chainActive.Height() < 12) return;

    vin = vinNew;
    blockHash = chainActive[chainActive.Height() - 12]->GetBlockHash();
    sigTime = GetAdjustedTime();
    vchSig = std::vector < unsigned char > ();
}

bool CEnodePing::Sign(CKey &keyEnode, CPubKey &pubKeyEnode) {
    std::string strError;
    std::string strZNodeSignMessage;

    sigTime = GetAdjustedTime();
    std::string strMessage = vin.ToString() + blockHash.ToString() + boost::lexical_cast<std::string>(sigTime);

    if (!darkSendSigner.SignMessage(strMessage, vchSig, keyEnode)) {
        LogPrintf("CEnodePing::Sign -- SignMessage() failed\n");
        return false;
    }

    if (!darkSendSigner.VerifyMessage(pubKeyEnode, vchSig, strMessage, strError)) {
        LogPrintf("CEnodePing::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CEnodePing::CheckSignature(CPubKey &pubKeyEnode, int &nDos) {
    std::string strMessage = vin.ToString() + blockHash.ToString() + boost::lexical_cast<std::string>(sigTime);
    std::string strError = "";
    nDos = 0;

    if (!darkSendSigner.VerifyMessage(pubKeyEnode, vchSig, strMessage, strError)) {
        LogPrintf("CEnodePing::CheckSignature -- Got bad Enode ping signature, znode=%s, error: %s\n", vin.prevout.ToStringShort(), strError);
        nDos = 33;
        return false;
    }
    return true;
}

bool CEnodePing::SimpleCheck(int &nDos) {
    // don't ban by default
    nDos = 0;

    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrintf("CEnodePing::SimpleCheck -- Signature rejected, too far into the future, znode=%s\n", vin.prevout.ToStringShort());
        nDos = 1;
        return false;
    }

    {
//        LOCK(cs_main);
        AssertLockHeld(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(blockHash);
        if (mi == mapBlockIndex.end()) {
            LogPrint("znode", "CEnodePing::SimpleCheck -- Enode ping is invalid, unknown block hash: znode=%s blockHash=%s\n", vin.prevout.ToStringShort(), blockHash.ToString());
            // maybe we stuck or forked so we shouldn't ban this node, just fail to accept this ping
            // TODO: or should we also request this block?
            return false;
        }
    }
    LogPrint("znode", "CEnodePing::SimpleCheck -- Enode ping verified: znode=%s  blockHash=%s  sigTime=%d\n", vin.prevout.ToStringShort(), blockHash.ToString(), sigTime);
    return true;
}

bool CEnodePing::CheckAndUpdate(CEnode *pmn, bool fFromNewBroadcast, int &nDos) {
    // don't ban by default
    nDos = 0;

    if (!SimpleCheck(nDos)) {
        return false;
    }

    if (pmn == NULL) {
        LogPrint("znode", "CEnodePing::CheckAndUpdate -- Couldn't find Enode entry, znode=%s\n", vin.prevout.ToStringShort());
        return false;
    }

    if (!fFromNewBroadcast) {
        if (pmn->IsUpdateRequired()) {
            LogPrint("znode", "CEnodePing::CheckAndUpdate -- znode protocol is outdated, znode=%s\n", vin.prevout.ToStringShort());
            return false;
        }

        if (pmn->IsNewStartRequired()) {
            LogPrint("znode", "CEnodePing::CheckAndUpdate -- znode is completely expired, new start is required, znode=%s\n", vin.prevout.ToStringShort());
            return false;
        }
    }

    {
        LOCK(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(blockHash);
        if ((*mi).second && (*mi).second->nHeight < chainActive.Height() - 24) {
            LogPrintf("CEnodePing::CheckAndUpdate -- Enode ping is invalid, block hash is too old: znode=%s  blockHash=%s\n", vin.prevout.ToStringShort(), blockHash.ToString());
            // nDos = 1;
            return false;
        }
    }

    LogPrint("znode", "CEnodePing::CheckAndUpdate -- New ping: znode=%s  blockHash=%s  sigTime=%d\n", vin.prevout.ToStringShort(), blockHash.ToString(), sigTime);

    // LogPrintf("mnping - Found corresponding mn for vin: %s\n", vin.prevout.ToStringShort());
    // update only if there is no known ping for this znode or
    // last ping was more then ENODE_MIN_MNP_SECONDS-60 ago comparing to this one
    if (pmn->IsPingedWithin(ENODE_MIN_MNP_SECONDS - 60, sigTime)) {
        LogPrint("znode", "CEnodePing::CheckAndUpdate -- Enode ping arrived too early, znode=%s\n", vin.prevout.ToStringShort());
        //nDos = 1; //disable, this is happening frequently and causing banned peers
        return false;
    }

    if (!CheckSignature(pmn->pubKeyEnode, nDos)) return false;

    // so, ping seems to be ok

    // if we are still syncing and there was no known ping for this mn for quite a while
    // (NOTE: assuming that ENODE_EXPIRATION_SECONDS/2 should be enough to finish mn list sync)
    if (!znodeSync.IsEnodeListSynced() && !pmn->IsPingedWithin(ENODE_EXPIRATION_SECONDS / 2)) {
        // let's bump sync timeout
        LogPrint("znode", "CEnodePing::CheckAndUpdate -- bumping sync timeout, znode=%s\n", vin.prevout.ToStringShort());
        znodeSync.AddedEnodeList();
    }

    // let's store this ping as the last one
    LogPrint("znode", "CEnodePing::CheckAndUpdate -- Enode ping accepted, znode=%s\n", vin.prevout.ToStringShort());
    pmn->lastPing = *this;

    // and update mnodeman.mapSeenEnodeBroadcast.lastPing which is probably outdated
    CEnodeBroadcast mnb(*pmn);
    uint256 hash = mnb.GetHash();
    if (mnodeman.mapSeenEnodeBroadcast.count(hash)) {
        mnodeman.mapSeenEnodeBroadcast[hash].second.lastPing = *this;
    }

    pmn->Check(true); // force update, ignoring cache
    if (!pmn->IsEnabled()) return false;

    LogPrint("znode", "CEnodePing::CheckAndUpdate -- Enode ping acceepted and relayed, znode=%s\n", vin.prevout.ToStringShort());
    Relay();

    return true;
}

void CEnodePing::Relay() {
    CInv inv(MSG_ENODE_PING, GetHash());
    RelayInv(inv);
}

//void CEnode::AddGovernanceVote(uint256 nGovernanceObjectHash)
//{
//    if(mapGovernanceObjectsVotedOn.count(nGovernanceObjectHash)) {
//        mapGovernanceObjectsVotedOn[nGovernanceObjectHash]++;
//    } else {
//        mapGovernanceObjectsVotedOn.insert(std::make_pair(nGovernanceObjectHash, 1));
//    }
//}

//void CEnode::RemoveGovernanceObject(uint256 nGovernanceObjectHash)
//{
//    std::map<uint256, int>::iterator it = mapGovernanceObjectsVotedOn.find(nGovernanceObjectHash);
//    if(it == mapGovernanceObjectsVotedOn.end()) {
//        return;
//    }
//    mapGovernanceObjectsVotedOn.erase(it);
//}

void CEnode::UpdateWatchdogVoteTime() {
    LOCK(cs);
    nTimeLastWatchdogVote = GetTime();
}

/**
*   FLAG GOVERNANCE ITEMS AS DIRTY
*
*   - When znode come and go on the network, we must flag the items they voted on to recalc it's cached flags
*
*/
//void CEnode::FlagGovernanceItemsAsDirty()
//{
//    std::vector<uint256> vecDirty;
//    {
//        std::map<uint256, int>::iterator it = mapGovernanceObjectsVotedOn.begin();
//        while(it != mapGovernanceObjectsVotedOn.end()) {
//            vecDirty.push_back(it->first);
//            ++it;
//        }
//    }
//    for(size_t i = 0; i < vecDirty.size(); ++i) {
//        mnodeman.AddDirtyGovernanceObjectHash(vecDirty[i]);
//    }
//}
