// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activeznode.h"
#include "addrman.h"
#include "darksend.h"
//#include "governance.h"
#include "znode-payments.h"
#include "znode-sync.h"
#include "znodeman.h"
#include "netfulfilledman.h"
#include "util.h"

/** Enode manager */
CEnodeMan mnodeman;

const std::string CEnodeMan::SERIALIZATION_VERSION_STRING = "CEnodeMan-Version-4";

struct CompareLastPaidBlock
{
    bool operator()(const std::pair<int, CEnode*>& t1,
                    const std::pair<int, CEnode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->vin < t2.second->vin);
    }
};

struct CompareScoreMN
{
    bool operator()(const std::pair<int64_t, CEnode*>& t1,
                    const std::pair<int64_t, CEnode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->vin < t2.second->vin);
    }
};

CEnodeIndex::CEnodeIndex()
    : nSize(0),
      mapIndex(),
      mapReverseIndex()
{}

bool CEnodeIndex::Get(int nIndex, CTxIn& vinEnode) const
{
    rindex_m_cit it = mapReverseIndex.find(nIndex);
    if(it == mapReverseIndex.end()) {
        return false;
    }
    vinEnode = it->second;
    return true;
}

int CEnodeIndex::GetEnodeIndex(const CTxIn& vinEnode) const
{
    index_m_cit it = mapIndex.find(vinEnode);
    if(it == mapIndex.end()) {
        return -1;
    }
    return it->second;
}

void CEnodeIndex::AddEnodeVIN(const CTxIn& vinEnode)
{
    index_m_it it = mapIndex.find(vinEnode);
    if(it != mapIndex.end()) {
        return;
    }
    int nNextIndex = nSize;
    mapIndex[vinEnode] = nNextIndex;
    mapReverseIndex[nNextIndex] = vinEnode;
    ++nSize;
}

void CEnodeIndex::Clear()
{
    mapIndex.clear();
    mapReverseIndex.clear();
    nSize = 0;
}
struct CompareByAddr

{
    bool operator()(const CEnode* t1,
                    const CEnode* t2) const
    {
        return t1->addr < t2->addr;
    }
};

void CEnodeIndex::RebuildIndex()
{
    nSize = mapIndex.size();
    for(index_m_it it = mapIndex.begin(); it != mapIndex.end(); ++it) {
        mapReverseIndex[it->second] = it->first;
    }
}

CEnodeMan::CEnodeMan() : cs(),
  vEnodes(),
  mAskedUsForEnodeList(),
  mWeAskedForEnodeList(),
  mWeAskedForEnodeListEntry(),
  mWeAskedForVerification(),
  mMnbRecoveryRequests(),
  mMnbRecoveryGoodReplies(),
  listScheduledMnbRequestConnections(),
  nLastIndexRebuildTime(0),
  indexEnodes(),
  indexEnodesOld(),
  fIndexRebuilt(false),
  fEnodesAdded(false),
  fEnodesRemoved(false),
//  vecDirtyGovernanceObjectHashes(),
  nLastWatchdogVoteTime(0),
  mapSeenEnodeBroadcast(),
  mapSeenEnodePing(),
  nDsqCount(0)
{}

bool CEnodeMan::Add(CEnode &mn)
{
    LOCK(cs);

    CEnode *pmn = Find(mn.vin);
    if (pmn == NULL) {
        LogPrint("znode", "CEnodeMan::Add -- Adding new Enode: addr=%s, %i now\n", mn.addr.ToString(), size() + 1);
        vEnodes.push_back(mn);
        indexEnodes.AddEnodeVIN(mn.vin);
        fEnodesAdded = true;
        return true;
    }

    return false;
}

void CEnodeMan::AskForMN(CNode* pnode, const CTxIn &vin)
{
    if(!pnode) return;

    LOCK(cs);

    std::map<COutPoint, std::map<CNetAddr, int64_t> >::iterator it1 = mWeAskedForEnodeListEntry.find(vin.prevout);
    if (it1 != mWeAskedForEnodeListEntry.end()) {
        std::map<CNetAddr, int64_t>::iterator it2 = it1->second.find(pnode->addr);
        if (it2 != it1->second.end()) {
            if (GetTime() < it2->second) {
                // we've asked recently, should not repeat too often or we could get banned
                return;
            }
            // we asked this node for this outpoint but it's ok to ask again already
            LogPrintf("CEnodeMan::AskForMN -- Asking same peer %s for missing znode entry again: %s\n", pnode->addr.ToString(), vin.prevout.ToStringShort());
        } else {
            // we already asked for this outpoint but not this node
            LogPrintf("CEnodeMan::AskForMN -- Asking new peer %s for missing znode entry: %s\n", pnode->addr.ToString(), vin.prevout.ToStringShort());
        }
    } else {
        // we never asked any node for this outpoint
        LogPrintf("CEnodeMan::AskForMN -- Asking peer %s for missing znode entry for the first time: %s\n", pnode->addr.ToString(), vin.prevout.ToStringShort());
    }
    mWeAskedForEnodeListEntry[vin.prevout][pnode->addr] = GetTime() + DSEG_UPDATE_SECONDS;

    pnode->PushMessage(NetMsgType::DSEG, vin);
}

void CEnodeMan::Check()
{
    LOCK(cs);

//    LogPrint("znode", "CEnodeMan::Check -- nLastWatchdogVoteTime=%d, IsWatchdogActive()=%d\n", nLastWatchdogVoteTime, IsWatchdogActive());

    BOOST_FOREACH(CEnode& mn, vEnodes) {
        mn.Check();
    }
}

void CEnodeMan::CheckAndRemove()
{
    if(!znodeSync.IsEnodeListSynced()) return;

    LogPrintf("CEnodeMan::CheckAndRemove\n");

    {
        // Need LOCK2 here to ensure consistent locking order because code below locks cs_main
        // in CheckMnbAndUpdateEnodeList()
        LOCK2(cs_main, cs);

        Check();

        // Remove spent znodes, prepare structures and make requests to reasure the state of inactive ones
        std::vector<CEnode>::iterator it = vEnodes.begin();
        std::vector<std::pair<int, CEnode> > vecEnodeRanks;
        // ask for up to MNB_RECOVERY_MAX_ASK_ENTRIES znode entries at a time
        int nAskForMnbRecovery = MNB_RECOVERY_MAX_ASK_ENTRIES;
        while(it != vEnodes.end()) {
            CEnodeBroadcast mnb = CEnodeBroadcast(*it);
            uint256 hash = mnb.GetHash();
            // If collateral was spent ...
            if ((*it).IsOutpointSpent()) {
                LogPrint("znode", "CEnodeMan::CheckAndRemove -- Removing Enode: %s  addr=%s  %i now\n", (*it).GetStateString(), (*it).addr.ToString(), size() - 1);

                // erase all of the broadcasts we've seen from this txin, ...
                mapSeenEnodeBroadcast.erase(hash);
                mWeAskedForEnodeListEntry.erase((*it).vin.prevout);

                // and finally remove it from the list
//                it->FlagGovernanceItemsAsDirty();
                it = vEnodes.erase(it);
                fEnodesRemoved = true;
            } else {
                bool fAsk = pCurrentBlockIndex &&
                            (nAskForMnbRecovery > 0) &&
                            znodeSync.IsSynced() &&
                            it->IsNewStartRequired() &&
                            !IsMnbRecoveryRequested(hash);
                if(fAsk) {
                    // this mn is in a non-recoverable state and we haven't asked other nodes yet
                    std::set<CNetAddr> setRequested;
                    // calulate only once and only when it's needed
                    if(vecEnodeRanks.empty()) {
                        int nRandomBlockHeight = GetRandInt(pCurrentBlockIndex->nHeight);
                        vecEnodeRanks = GetEnodeRanks(nRandomBlockHeight);
                    }
                    bool fAskedForMnbRecovery = false;
                    // ask first MNB_RECOVERY_QUORUM_TOTAL znodes we can connect to and we haven't asked recently
                    for(int i = 0; setRequested.size() < MNB_RECOVERY_QUORUM_TOTAL && i < (int)vecEnodeRanks.size(); i++) {
                        // avoid banning
                        if(mWeAskedForEnodeListEntry.count(it->vin.prevout) && mWeAskedForEnodeListEntry[it->vin.prevout].count(vecEnodeRanks[i].second.addr)) continue;
                        // didn't ask recently, ok to ask now
                        CService addr = vecEnodeRanks[i].second.addr;
                        setRequested.insert(addr);
                        listScheduledMnbRequestConnections.push_back(std::make_pair(addr, hash));
                        fAskedForMnbRecovery = true;
                    }
                    if(fAskedForMnbRecovery) {
                        LogPrint("znode", "CEnodeMan::CheckAndRemove -- Recovery initiated, znode=%s\n", it->vin.prevout.ToStringShort());
                        nAskForMnbRecovery--;
                    }
                    // wait for mnb recovery replies for MNB_RECOVERY_WAIT_SECONDS seconds
                    mMnbRecoveryRequests[hash] = std::make_pair(GetTime() + MNB_RECOVERY_WAIT_SECONDS, setRequested);
                }
                ++it;
            }
        }

        // proces replies for ENODE_NEW_START_REQUIRED znodes
        LogPrint("znode", "CEnodeMan::CheckAndRemove -- mMnbRecoveryGoodReplies size=%d\n", (int)mMnbRecoveryGoodReplies.size());
        std::map<uint256, std::vector<CEnodeBroadcast> >::iterator itMnbReplies = mMnbRecoveryGoodReplies.begin();
        while(itMnbReplies != mMnbRecoveryGoodReplies.end()){
            if(mMnbRecoveryRequests[itMnbReplies->first].first < GetTime()) {
                // all nodes we asked should have replied now
                if(itMnbReplies->second.size() >= MNB_RECOVERY_QUORUM_REQUIRED) {
                    // majority of nodes we asked agrees that this mn doesn't require new mnb, reprocess one of new mnbs
                    LogPrint("znode", "CEnodeMan::CheckAndRemove -- reprocessing mnb, znode=%s\n", itMnbReplies->second[0].vin.prevout.ToStringShort());
                    // mapSeenEnodeBroadcast.erase(itMnbReplies->first);
                    int nDos;
                    itMnbReplies->second[0].fRecovery = true;
                    CheckMnbAndUpdateEnodeList(NULL, itMnbReplies->second[0], nDos);
                }
                LogPrint("znode", "CEnodeMan::CheckAndRemove -- removing mnb recovery reply, znode=%s, size=%d\n", itMnbReplies->second[0].vin.prevout.ToStringShort(), (int)itMnbReplies->second.size());
                mMnbRecoveryGoodReplies.erase(itMnbReplies++);
            } else {
                ++itMnbReplies;
            }
        }
    }
    {
        // no need for cm_main below
        LOCK(cs);

        std::map<uint256, std::pair< int64_t, std::set<CNetAddr> > >::iterator itMnbRequest = mMnbRecoveryRequests.begin();
        while(itMnbRequest != mMnbRecoveryRequests.end()){
            // Allow this mnb to be re-verified again after MNB_RECOVERY_RETRY_SECONDS seconds
            // if mn is still in ENODE_NEW_START_REQUIRED state.
            if(GetTime() - itMnbRequest->second.first > MNB_RECOVERY_RETRY_SECONDS) {
                mMnbRecoveryRequests.erase(itMnbRequest++);
            } else {
                ++itMnbRequest;
            }
        }

        // check who's asked for the Enode list
        std::map<CNetAddr, int64_t>::iterator it1 = mAskedUsForEnodeList.begin();
        while(it1 != mAskedUsForEnodeList.end()){
            if((*it1).second < GetTime()) {
                mAskedUsForEnodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check who we asked for the Enode list
        it1 = mWeAskedForEnodeList.begin();
        while(it1 != mWeAskedForEnodeList.end()){
            if((*it1).second < GetTime()){
                mWeAskedForEnodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check which Enodes we've asked for
        std::map<COutPoint, std::map<CNetAddr, int64_t> >::iterator it2 = mWeAskedForEnodeListEntry.begin();
        while(it2 != mWeAskedForEnodeListEntry.end()){
            std::map<CNetAddr, int64_t>::iterator it3 = it2->second.begin();
            while(it3 != it2->second.end()){
                if(it3->second < GetTime()){
                    it2->second.erase(it3++);
                } else {
                    ++it3;
                }
            }
            if(it2->second.empty()) {
                mWeAskedForEnodeListEntry.erase(it2++);
            } else {
                ++it2;
            }
        }

        std::map<CNetAddr, CEnodeVerification>::iterator it3 = mWeAskedForVerification.begin();
        while(it3 != mWeAskedForVerification.end()){
            if(it3->second.nBlockHeight < pCurrentBlockIndex->nHeight - MAX_POSE_BLOCKS) {
                mWeAskedForVerification.erase(it3++);
            } else {
                ++it3;
            }
        }

        // NOTE: do not expire mapSeenEnodeBroadcast entries here, clean them on mnb updates!

        // remove expired mapSeenEnodePing
        std::map<uint256, CEnodePing>::iterator it4 = mapSeenEnodePing.begin();
        while(it4 != mapSeenEnodePing.end()){
            if((*it4).second.IsExpired()) {
                LogPrint("znode", "CEnodeMan::CheckAndRemove -- Removing expired Enode ping: hash=%s\n", (*it4).second.GetHash().ToString());
                mapSeenEnodePing.erase(it4++);
            } else {
                ++it4;
            }
        }

        // remove expired mapSeenEnodeVerification
        std::map<uint256, CEnodeVerification>::iterator itv2 = mapSeenEnodeVerification.begin();
        while(itv2 != mapSeenEnodeVerification.end()){
            if((*itv2).second.nBlockHeight < pCurrentBlockIndex->nHeight - MAX_POSE_BLOCKS){
                LogPrint("znode", "CEnodeMan::CheckAndRemove -- Removing expired Enode verification: hash=%s\n", (*itv2).first.ToString());
                mapSeenEnodeVerification.erase(itv2++);
            } else {
                ++itv2;
            }
        }

        LogPrintf("CEnodeMan::CheckAndRemove -- %s\n", ToString());

        if(fEnodesRemoved) {
            CheckAndRebuildEnodeIndex();
        }
    }

    if(fEnodesRemoved) {
        NotifyEnodeUpdates();
    }
}

void CEnodeMan::Clear()
{
    LOCK(cs);
    vEnodes.clear();
    mAskedUsForEnodeList.clear();
    mWeAskedForEnodeList.clear();
    mWeAskedForEnodeListEntry.clear();
    mapSeenEnodeBroadcast.clear();
    mapSeenEnodePing.clear();
    nDsqCount = 0;
    nLastWatchdogVoteTime = 0;
    indexEnodes.Clear();
    indexEnodesOld.Clear();
}

int CEnodeMan::CountEnodes(int nProtocolVersion)
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinEnodePaymentsProto() : nProtocolVersion;

    BOOST_FOREACH(CEnode& mn, vEnodes) {
        if(mn.nProtocolVersion < nProtocolVersion) continue;
        nCount++;
    }

    return nCount;
}

int CEnodeMan::CountEnabled(int nProtocolVersion)
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinEnodePaymentsProto() : nProtocolVersion;

    BOOST_FOREACH(CEnode& mn, vEnodes) {
        if(mn.nProtocolVersion < nProtocolVersion || !mn.IsEnabled()) continue;
        nCount++;
    }

    return nCount;
}

/* Only IPv4 znodes are allowed in 12.1, saving this for later
int CEnodeMan::CountByIP(int nNetworkType)
{
    LOCK(cs);
    int nNodeCount = 0;

    BOOST_FOREACH(CEnode& mn, vEnodes)
        if ((nNetworkType == NET_IPV4 && mn.addr.IsIPv4()) ||
            (nNetworkType == NET_TOR  && mn.addr.IsTor())  ||
            (nNetworkType == NET_IPV6 && mn.addr.IsIPv6())) {
                nNodeCount++;
        }

    return nNodeCount;
}
*/

void CEnodeMan::DsegUpdate(CNode* pnode)
{
    LOCK(cs);

    if(Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if(!(pnode->addr.IsRFC1918() || pnode->addr.IsLocal())) {
            std::map<CNetAddr, int64_t>::iterator it = mWeAskedForEnodeList.find(pnode->addr);
            if(it != mWeAskedForEnodeList.end() && GetTime() < (*it).second) {
                LogPrintf("CEnodeMan::DsegUpdate -- we already asked %s for the list; skipping...\n", pnode->addr.ToString());
                return;
            }
        }
    }
    
    pnode->PushMessage(NetMsgType::DSEG, CTxIn());
    int64_t askAgain = GetTime() + DSEG_UPDATE_SECONDS;
    mWeAskedForEnodeList[pnode->addr] = askAgain;

    LogPrint("znode", "CEnodeMan::DsegUpdate -- asked %s for the list\n", pnode->addr.ToString());
}

CEnode* CEnodeMan::Find(const CScript &payee)
{
    LOCK(cs);

    BOOST_FOREACH(CEnode& mn, vEnodes)
    {
        if(GetScriptForDestination(mn.pubKeyCollateralAddress.GetID()) == payee)
            return &mn;
    }
    return NULL;
}

CEnode* CEnodeMan::Find(const CTxIn &vin)
{
    LOCK(cs);

    BOOST_FOREACH(CEnode& mn, vEnodes)
    {
        if(mn.vin.prevout == vin.prevout)
            return &mn;
    }
    return NULL;
}

CEnode* CEnodeMan::Find(const CPubKey &pubKeyEnode)
{
    LOCK(cs);

    BOOST_FOREACH(CEnode& mn, vEnodes)
    {
        if(mn.pubKeyEnode == pubKeyEnode)
            return &mn;
    }
    return NULL;
}

bool CEnodeMan::Get(const CPubKey& pubKeyEnode, CEnode& znode)
{
    // Theses mutexes are recursive so double locking by the same thread is safe.
    LOCK(cs);
    CEnode* pMN = Find(pubKeyEnode);
    if(!pMN)  {
        return false;
    }
    znode = *pMN;
    return true;
}

bool CEnodeMan::Get(const CTxIn& vin, CEnode& znode)
{
    // Theses mutexes are recursive so double locking by the same thread is safe.
    LOCK(cs);
    CEnode* pMN = Find(vin);
    if(!pMN)  {
        return false;
    }
    znode = *pMN;
    return true;
}

znode_info_t CEnodeMan::GetEnodeInfo(const CTxIn& vin)
{
    znode_info_t info;
    LOCK(cs);
    CEnode* pMN = Find(vin);
    if(!pMN)  {
        return info;
    }
    info = pMN->GetInfo();
    return info;
}

znode_info_t CEnodeMan::GetEnodeInfo(const CPubKey& pubKeyEnode)
{
    znode_info_t info;
    LOCK(cs);
    CEnode* pMN = Find(pubKeyEnode);
    if(!pMN)  {
        return info;
    }
    info = pMN->GetInfo();
    return info;
}

bool CEnodeMan::Has(const CTxIn& vin)
{
    LOCK(cs);
    CEnode* pMN = Find(vin);
    return (pMN != NULL);
}

char* CEnodeMan::GetNotQualifyReason(CEnode& mn, int nBlockHeight, bool fFilterSigTime, int nMnCount)
{
    if (!mn.IsValidForPayment()) {
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'not valid for payment'");
        return reasonStr;
    }
    // //check protocol version
    if (mn.nProtocolVersion < mnpayments.GetMinEnodePaymentsProto()) {
        // LogPrintf("Invalid nProtocolVersion!\n");
        // LogPrintf("mn.nProtocolVersion=%s!\n", mn.nProtocolVersion);
        // LogPrintf("mnpayments.GetMinEnodePaymentsProto=%s!\n", mnpayments.GetMinEnodePaymentsProto());
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'Invalid nProtocolVersion', nProtocolVersion=%d", mn.nProtocolVersion);
        return reasonStr;
    }
    //it's in the list (up to 8 entries ahead of current block to allow propagation) -- so let's skip it
    if (mnpayments.IsScheduled(mn, nBlockHeight)) {
        // LogPrintf("mnpayments.IsScheduled!\n");
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'is scheduled'");
        return reasonStr;
    }
    //it's too new, wait for a cycle
    if (fFilterSigTime && mn.sigTime + (nMnCount * 2.6 * 60) > GetAdjustedTime()) {
        // LogPrintf("it's too new, wait for a cycle!\n");
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'too new', sigTime=%s, will be qualifed after=%s",
                DateTimeStrFormat("%Y-%m-%d %H:%M UTC", mn.sigTime).c_str(), DateTimeStrFormat("%Y-%m-%d %H:%M UTC", mn.sigTime + (nMnCount * 2.6 * 60)).c_str());
        return reasonStr;
    }
    //make sure it has at least as many confirmations as there are znodes
    if (mn.GetCollateralAge() < nMnCount) {
        // LogPrintf("mn.GetCollateralAge()=%s!\n", mn.GetCollateralAge());
        // LogPrintf("nMnCount=%s!\n", nMnCount);
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'collateralAge < znCount', collateralAge=%d, znCount=%d", mn.GetCollateralAge(), nMnCount);
        return reasonStr;
    }
    return NULL;
}

//
// Deterministically select the oldest/best znode to pay on the network
//
CEnode* CEnodeMan::GetNextEnodeInQueueForPayment(bool fFilterSigTime, int& nCount)
{
    if(!pCurrentBlockIndex) {
        nCount = 0;
        return NULL;
    }
    return GetNextEnodeInQueueForPayment(pCurrentBlockIndex->nHeight, fFilterSigTime, nCount);
}

CEnode* CEnodeMan::GetNextEnodeInQueueForPayment(int nBlockHeight, bool fFilterSigTime, int& nCount)
{
    // Need LOCK2 here to ensure consistent locking order because the GetBlockHash call below locks cs_main
    LOCK2(cs_main,cs);

    CEnode *pBestEnode = NULL;
    std::vector<std::pair<int, CEnode*> > vecEnodeLastPaid;

    /*
        Make a vector with all of the last paid times
    */
    int nMnCount = CountEnabled();
    int index = 0;
    BOOST_FOREACH(CEnode &mn, vEnodes)
    {
        index += 1;
        // LogPrintf("index=%s, mn=%s\n", index, mn.ToString());
        /*if (!mn.IsValidForPayment()) {
            LogPrint("znodeman", "Enode, %s, addr(%s), not-qualified: 'not valid for payment'\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString());
            continue;
        }
        // //check protocol version
        if (mn.nProtocolVersion < mnpayments.GetMinEnodePaymentsProto()) {
            // LogPrintf("Invalid nProtocolVersion!\n");
            // LogPrintf("mn.nProtocolVersion=%s!\n", mn.nProtocolVersion);
            // LogPrintf("mnpayments.GetMinEnodePaymentsProto=%s!\n", mnpayments.GetMinEnodePaymentsProto());
            LogPrint("znodeman", "Enode, %s, addr(%s), not-qualified: 'invalid nProtocolVersion'\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString());
            continue;
        }
        //it's in the list (up to 8 entries ahead of current block to allow propagation) -- so let's skip it
        if (mnpayments.IsScheduled(mn, nBlockHeight)) {
            // LogPrintf("mnpayments.IsScheduled!\n");
            LogPrint("znodeman", "Enode, %s, addr(%s), not-qualified: 'IsScheduled'\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString());
            continue;
        }
        //it's too new, wait for a cycle
        if (fFilterSigTime && mn.sigTime + (nMnCount * 2.6 * 60) > GetAdjustedTime()) {
            // LogPrintf("it's too new, wait for a cycle!\n");
            LogPrint("znodeman", "Enode, %s, addr(%s), not-qualified: 'it's too new, wait for a cycle!', sigTime=%s, will be qualifed after=%s\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString(), DateTimeStrFormat("%Y-%m-%d %H:%M UTC", mn.sigTime).c_str(), DateTimeStrFormat("%Y-%m-%d %H:%M UTC", mn.sigTime + (nMnCount * 2.6 * 60)).c_str());
            continue;
        }
        //make sure it has at least as many confirmations as there are znodes
        if (mn.GetCollateralAge() < nMnCount) {
            // LogPrintf("mn.GetCollateralAge()=%s!\n", mn.GetCollateralAge());
            // LogPrintf("nMnCount=%s!\n", nMnCount);
            LogPrint("znodeman", "Enode, %s, addr(%s), not-qualified: 'mn.GetCollateralAge() < nMnCount', CollateralAge=%d, nMnCount=%d\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString(), mn.GetCollateralAge(), nMnCount);
            continue;
        }*/
        char* reasonStr = GetNotQualifyReason(mn, nBlockHeight, fFilterSigTime, nMnCount);
        if (reasonStr != NULL) {
            LogPrint("znodeman", "Enode, %s, addr(%s), qualify %s\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString(), reasonStr);
            delete [] reasonStr;
            continue;
        }
        vecEnodeLastPaid.push_back(std::make_pair(mn.GetLastPaidBlock(), &mn));
    }
    nCount = (int)vecEnodeLastPaid.size();

    //when the network is in the process of upgrading, don't penalize nodes that recently restarted
    if(fFilterSigTime && nCount < nMnCount / 3) {
        // LogPrintf("Need Return, nCount=%s, nMnCount/3=%s\n", nCount, nMnCount/3);
        return GetNextEnodeInQueueForPayment(nBlockHeight, false, nCount);
    }

    // Sort them low to high
    sort(vecEnodeLastPaid.begin(), vecEnodeLastPaid.end(), CompareLastPaidBlock());

    uint256 blockHash;
    if(!GetBlockHash(blockHash, nBlockHeight - 101)) {
        LogPrintf("CEnode::GetNextEnodeInQueueForPayment -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", nBlockHeight - 101);
        return NULL;
    }
    // Look at 1/10 of the oldest nodes (by last payment), calculate their scores and pay the best one
    //  -- This doesn't look at who is being paid in the +8-10 blocks, allowing for double payments very rarely
    //  -- 1/100 payments should be a double payment on mainnet - (1/(3000/10))*2
    //  -- (chance per block * chances before IsScheduled will fire)
    int nTenthNetwork = nMnCount/10;
    int nCountTenth = 0;
    arith_uint256 nHighest = 0;
    BOOST_FOREACH (PAIRTYPE(int, CEnode*)& s, vecEnodeLastPaid){
        arith_uint256 nScore = s.second->CalculateScore(blockHash);
        if(nScore > nHighest){
            nHighest = nScore;
            pBestEnode = s.second;
        }
        nCountTenth++;
        if(nCountTenth >= nTenthNetwork) break;
    }
    return pBestEnode;
}

CEnode* CEnodeMan::FindRandomNotInVec(const std::vector<CTxIn> &vecToExclude, int nProtocolVersion)
{
    LOCK(cs);

    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinEnodePaymentsProto() : nProtocolVersion;

    int nCountEnabled = CountEnabled(nProtocolVersion);
    int nCountNotExcluded = nCountEnabled - vecToExclude.size();

    LogPrintf("CEnodeMan::FindRandomNotInVec -- %d enabled znodes, %d znodes to choose from\n", nCountEnabled, nCountNotExcluded);
    if(nCountNotExcluded < 1) return NULL;

    // fill a vector of pointers
    std::vector<CEnode*> vpEnodesShuffled;
    BOOST_FOREACH(CEnode &mn, vEnodes) {
        vpEnodesShuffled.push_back(&mn);
    }

    InsecureRand insecureRand;
    // shuffle pointers
    std::random_shuffle(vpEnodesShuffled.begin(), vpEnodesShuffled.end(), insecureRand);
    bool fExclude;

    // loop through
    BOOST_FOREACH(CEnode* pmn, vpEnodesShuffled) {
        if(pmn->nProtocolVersion < nProtocolVersion || !pmn->IsEnabled()) continue;
        fExclude = false;
        BOOST_FOREACH(const CTxIn &txinToExclude, vecToExclude) {
            if(pmn->vin.prevout == txinToExclude.prevout) {
                fExclude = true;
                break;
            }
        }
        if(fExclude) continue;
        // found the one not in vecToExclude
        LogPrint("znode", "CEnodeMan::FindRandomNotInVec -- found, znode=%s\n", pmn->vin.prevout.ToStringShort());
        return pmn;
    }

    LogPrint("znode", "CEnodeMan::FindRandomNotInVec -- failed\n");
    return NULL;
}

int CEnodeMan::GetEnodeRank(const CTxIn& vin, int nBlockHeight, int nMinProtocol, bool fOnlyActive)
{
    std::vector<std::pair<int64_t, CEnode*> > vecEnodeScores;

    //make sure we know about this block
    uint256 blockHash = uint256();
    if(!GetBlockHash(blockHash, nBlockHeight)) return -1;

    LOCK(cs);

    // scan for winner
    BOOST_FOREACH(CEnode& mn, vEnodes) {
        if(mn.nProtocolVersion < nMinProtocol) continue;
        if(fOnlyActive) {
            if(!mn.IsEnabled()) continue;
        }
        else {
            if(!mn.IsValidForPayment()) continue;
        }
        int64_t nScore = mn.CalculateScore(blockHash).GetCompact(false);

        vecEnodeScores.push_back(std::make_pair(nScore, &mn));
    }

    sort(vecEnodeScores.rbegin(), vecEnodeScores.rend(), CompareScoreMN());

    int nRank = 0;
    BOOST_FOREACH (PAIRTYPE(int64_t, CEnode*)& scorePair, vecEnodeScores) {
        nRank++;
        if(scorePair.second->vin.prevout == vin.prevout) return nRank;
    }

    return -1;
}

std::vector<std::pair<int, CEnode> > CEnodeMan::GetEnodeRanks(int nBlockHeight, int nMinProtocol)
{
    std::vector<std::pair<int64_t, CEnode*> > vecEnodeScores;
    std::vector<std::pair<int, CEnode> > vecEnodeRanks;

    //make sure we know about this block
    uint256 blockHash = uint256();
    if(!GetBlockHash(blockHash, nBlockHeight)) return vecEnodeRanks;

    LOCK(cs);

    // scan for winner
    BOOST_FOREACH(CEnode& mn, vEnodes) {

        if(mn.nProtocolVersion < nMinProtocol || !mn.IsEnabled()) continue;

        int64_t nScore = mn.CalculateScore(blockHash).GetCompact(false);

        vecEnodeScores.push_back(std::make_pair(nScore, &mn));
    }

    sort(vecEnodeScores.rbegin(), vecEnodeScores.rend(), CompareScoreMN());

    int nRank = 0;
    BOOST_FOREACH (PAIRTYPE(int64_t, CEnode*)& s, vecEnodeScores) {
        nRank++;
        vecEnodeRanks.push_back(std::make_pair(nRank, *s.second));
    }

    return vecEnodeRanks;
}

CEnode* CEnodeMan::GetEnodeByRank(int nRank, int nBlockHeight, int nMinProtocol, bool fOnlyActive)
{
    std::vector<std::pair<int64_t, CEnode*> > vecEnodeScores;

    LOCK(cs);

    uint256 blockHash;
    if(!GetBlockHash(blockHash, nBlockHeight)) {
        LogPrintf("CEnode::GetEnodeByRank -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", nBlockHeight);
        return NULL;
    }

    // Fill scores
    BOOST_FOREACH(CEnode& mn, vEnodes) {

        if(mn.nProtocolVersion < nMinProtocol) continue;
        if(fOnlyActive && !mn.IsEnabled()) continue;

        int64_t nScore = mn.CalculateScore(blockHash).GetCompact(false);

        vecEnodeScores.push_back(std::make_pair(nScore, &mn));
    }

    sort(vecEnodeScores.rbegin(), vecEnodeScores.rend(), CompareScoreMN());

    int rank = 0;
    BOOST_FOREACH (PAIRTYPE(int64_t, CEnode*)& s, vecEnodeScores){
        rank++;
        if(rank == nRank) {
            return s.second;
        }
    }

    return NULL;
}

void CEnodeMan::ProcessEnodeConnections()
{
    //we don't care about this for regtest
    if(Params().NetworkIDString() == CBaseChainParams::REGTEST) return;

    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes) {
        if(pnode->fEnode) {
            if(darkSendPool.pSubmittedToEnode != NULL && pnode->addr == darkSendPool.pSubmittedToEnode->addr) continue;
            // LogPrintf("Closing Enode connection: peer=%d, addr=%s\n", pnode->id, pnode->addr.ToString());
            pnode->fDisconnect = true;
        }
    }
}

std::pair<CService, std::set<uint256> > CEnodeMan::PopScheduledMnbRequestConnection()
{
    LOCK(cs);
    if(listScheduledMnbRequestConnections.empty()) {
        return std::make_pair(CService(), std::set<uint256>());
    }

    std::set<uint256> setResult;

    listScheduledMnbRequestConnections.sort();
    std::pair<CService, uint256> pairFront = listScheduledMnbRequestConnections.front();

    // squash hashes from requests with the same CService as the first one into setResult
    std::list< std::pair<CService, uint256> >::iterator it = listScheduledMnbRequestConnections.begin();
    while(it != listScheduledMnbRequestConnections.end()) {
        if(pairFront.first == it->first) {
            setResult.insert(it->second);
            it = listScheduledMnbRequestConnections.erase(it);
        } else {
            // since list is sorted now, we can be sure that there is no more hashes left
            // to ask for from this addr
            break;
        }
    }
    return std::make_pair(pairFront.first, setResult);
}


void CEnodeMan::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{

//    LogPrint("znode", "CEnodeMan::ProcessMessage, strCommand=%s\n", strCommand);
    if(fLiteMode) return; // disable all Dash specific functionality
    if(!znodeSync.IsBlockchainSynced()) return;

    if (strCommand == NetMsgType::MNANNOUNCE) { //Enode Broadcast
        CEnodeBroadcast mnb;
        vRecv >> mnb;

        pfrom->setAskFor.erase(mnb.GetHash());

        LogPrintf("MNANNOUNCE -- Enode announce, znode=%s\n", mnb.vin.prevout.ToStringShort());

        int nDos = 0;

        if (CheckMnbAndUpdateEnodeList(pfrom, mnb, nDos)) {
            // use announced Enode as a peer
            addrman.Add(CAddress(mnb.addr, NODE_NETWORK), pfrom->addr, 2*60*60);
        } else if(nDos > 0) {
            Misbehaving(pfrom->GetId(), nDos);
        }

        if(fEnodesAdded) {
            NotifyEnodeUpdates();
        }
    } else if (strCommand == NetMsgType::MNPING) { //Enode Ping

        CEnodePing mnp;
        vRecv >> mnp;

        uint256 nHash = mnp.GetHash();

        pfrom->setAskFor.erase(nHash);

        LogPrint("znode", "MNPING -- Enode ping, znode=%s\n", mnp.vin.prevout.ToStringShort());

        // Need LOCK2 here to ensure consistent locking order because the CheckAndUpdate call below locks cs_main
        LOCK2(cs_main, cs);

        if(mapSeenEnodePing.count(nHash)) return; //seen
        mapSeenEnodePing.insert(std::make_pair(nHash, mnp));

        LogPrint("znode", "MNPING -- Enode ping, znode=%s new\n", mnp.vin.prevout.ToStringShort());

        // see if we have this Enode
        CEnode* pmn = mnodeman.Find(mnp.vin);

        // too late, new MNANNOUNCE is required
        if(pmn && pmn->IsNewStartRequired()) return;

        int nDos = 0;
        if(mnp.CheckAndUpdate(pmn, false, nDos)) return;

        if(nDos > 0) {
            // if anything significant failed, mark that node
            Misbehaving(pfrom->GetId(), nDos);
        } else if(pmn != NULL) {
            // nothing significant failed, mn is a known one too
            return;
        }

        // something significant is broken or mn is unknown,
        // we might have to ask for a znode entry once
        AskForMN(pfrom, mnp.vin);

    } else if (strCommand == NetMsgType::DSEG) { //Get Enode list or specific entry
        // Ignore such requests until we are fully synced.
        // We could start processing this after enode list is synced
        // but this is a heavy one so it's better to finish sync first.
        if (!znodeSync.IsSynced()) return;

        CTxIn vin;
        vRecv >> vin;

        LogPrint("znode", "DSEG -- Enode list, znode=%s\n", vin.prevout.ToStringShort());

        LOCK(cs);

        if(vin == CTxIn()) { //only should ask for this once
            //local network
            bool isLocal = (pfrom->addr.IsRFC1918() || pfrom->addr.IsLocal());

            if(!isLocal && Params().NetworkIDString() == CBaseChainParams::MAIN) {
                std::map<CNetAddr, int64_t>::iterator i = mAskedUsForEnodeList.find(pfrom->addr);
                if (i != mAskedUsForEnodeList.end()){
                    int64_t t = (*i).second;
                    if (GetTime() < t) {
                        Misbehaving(pfrom->GetId(), 34);
                        LogPrintf("DSEG -- peer already asked me for the list, peer=%d\n", pfrom->id);
                        return;
                    }
                }
                int64_t askAgain = GetTime() + DSEG_UPDATE_SECONDS;
                mAskedUsForEnodeList[pfrom->addr] = askAgain;
            }
        } //else, asking for a specific node which is ok

        int nInvCount = 0;

        BOOST_FOREACH(CEnode& mn, vEnodes) {
            if (vin != CTxIn() && vin != mn.vin) continue; // asked for specific vin but we are not there yet
            if (mn.addr.IsRFC1918() || mn.addr.IsLocal()) continue; // do not send local network znode
            if (mn.IsUpdateRequired()) continue; // do not send outdated znodes

            LogPrint("znode", "DSEG -- Sending Enode entry: znode=%s  addr=%s\n", mn.vin.prevout.ToStringShort(), mn.addr.ToString());
            CEnodeBroadcast mnb = CEnodeBroadcast(mn);
            uint256 hash = mnb.GetHash();
            pfrom->PushInventory(CInv(MSG_ENODE_ANNOUNCE, hash));
            pfrom->PushInventory(CInv(MSG_ENODE_PING, mn.lastPing.GetHash()));
            nInvCount++;

            if (!mapSeenEnodeBroadcast.count(hash)) {
                mapSeenEnodeBroadcast.insert(std::make_pair(hash, std::make_pair(GetTime(), mnb)));
            }

            if (vin == mn.vin) {
                LogPrintf("DSEG -- Sent 1 Enode inv to peer %d\n", pfrom->id);
                return;
            }
        }

        if(vin == CTxIn()) {
            pfrom->PushMessage(NetMsgType::SYNCSTATUSCOUNT, ENODE_SYNC_LIST, nInvCount);
            LogPrintf("DSEG -- Sent %d Enode invs to peer %d\n", nInvCount, pfrom->id);
            return;
        }
        // smth weird happen - someone asked us for vin we have no idea about?
        LogPrint("znode", "DSEG -- No invs sent to peer %d\n", pfrom->id);

    } else if (strCommand == NetMsgType::MNVERIFY) { // Enode Verify

        // Need LOCK2 here to ensure consistent locking order because the all functions below call GetBlockHash which locks cs_main
        LOCK2(cs_main, cs);

        CEnodeVerification mnv;
        vRecv >> mnv;

        if(mnv.vchSig1.empty()) {
            // CASE 1: someone asked me to verify myself /IP we are using/
            SendVerifyReply(pfrom, mnv);
        } else if (mnv.vchSig2.empty()) {
            // CASE 2: we _probably_ got verification we requested from some znode
            ProcessVerifyReply(pfrom, mnv);
        } else {
            // CASE 3: we _probably_ got verification broadcast signed by some znode which verified another one
            ProcessVerifyBroadcast(pfrom, mnv);
        }
    }
}

// Verification of znodes via unique direct requests.

void CEnodeMan::DoFullVerificationStep()
{
    if(activeEnode.vin == CTxIn()) return;
    if(!znodeSync.IsSynced()) return;

    std::vector<std::pair<int, CEnode> > vecEnodeRanks = GetEnodeRanks(pCurrentBlockIndex->nHeight - 1, MIN_POSE_PROTO_VERSION);

    // Need LOCK2 here to ensure consistent locking order because the SendVerifyRequest call below locks cs_main
    // through GetHeight() signal in ConnectNode
    LOCK2(cs_main, cs);

    int nCount = 0;

    int nMyRank = -1;
    int nRanksTotal = (int)vecEnodeRanks.size();

    // send verify requests only if we are in top MAX_POSE_RANK
    std::vector<std::pair<int, CEnode> >::iterator it = vecEnodeRanks.begin();
    while(it != vecEnodeRanks.end()) {
        if(it->first > MAX_POSE_RANK) {
            LogPrint("znode", "CEnodeMan::DoFullVerificationStep -- Must be in top %d to send verify request\n",
                        (int)MAX_POSE_RANK);
            return;
        }
        if(it->second.vin == activeEnode.vin) {
            nMyRank = it->first;
            LogPrint("znode", "CEnodeMan::DoFullVerificationStep -- Found self at rank %d/%d, verifying up to %d znodes\n",
                        nMyRank, nRanksTotal, (int)MAX_POSE_CONNECTIONS);
            break;
        }
        ++it;
    }

    // edge case: list is too short and this znode is not enabled
    if(nMyRank == -1) return;

    // send verify requests to up to MAX_POSE_CONNECTIONS znodes
    // starting from MAX_POSE_RANK + nMyRank and using MAX_POSE_CONNECTIONS as a step
    int nOffset = MAX_POSE_RANK + nMyRank - 1;
    if(nOffset >= (int)vecEnodeRanks.size()) return;

    std::vector<CEnode*> vSortedByAddr;
    BOOST_FOREACH(CEnode& mn, vEnodes) {
        vSortedByAddr.push_back(&mn);
    }

    sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());

    it = vecEnodeRanks.begin() + nOffset;
    while(it != vecEnodeRanks.end()) {
        if(it->second.IsPoSeVerified() || it->second.IsPoSeBanned()) {
            LogPrint("znode", "CEnodeMan::DoFullVerificationStep -- Already %s%s%s znode %s address %s, skipping...\n",
                        it->second.IsPoSeVerified() ? "verified" : "",
                        it->second.IsPoSeVerified() && it->second.IsPoSeBanned() ? " and " : "",
                        it->second.IsPoSeBanned() ? "banned" : "",
                        it->second.vin.prevout.ToStringShort(), it->second.addr.ToString());
            nOffset += MAX_POSE_CONNECTIONS;
            if(nOffset >= (int)vecEnodeRanks.size()) break;
            it += MAX_POSE_CONNECTIONS;
            continue;
        }
        LogPrint("znode", "CEnodeMan::DoFullVerificationStep -- Verifying znode %s rank %d/%d address %s\n",
                    it->second.vin.prevout.ToStringShort(), it->first, nRanksTotal, it->second.addr.ToString());
        if(SendVerifyRequest(CAddress(it->second.addr, NODE_NETWORK), vSortedByAddr)) {
            nCount++;
            if(nCount >= MAX_POSE_CONNECTIONS) break;
        }
        nOffset += MAX_POSE_CONNECTIONS;
        if(nOffset >= (int)vecEnodeRanks.size()) break;
        it += MAX_POSE_CONNECTIONS;
    }

    LogPrint("znode", "CEnodeMan::DoFullVerificationStep -- Sent verification requests to %d znodes\n", nCount);
}

// This function tries to find znodes with the same addr,
// find a verified one and ban all the other. If there are many nodes
// with the same addr but none of them is verified yet, then none of them are banned.
// It could take many times to run this before most of the duplicate nodes are banned.

void CEnodeMan::CheckSameAddr()
{
    if(!znodeSync.IsSynced() || vEnodes.empty()) return;

    std::vector<CEnode*> vBan;
    std::vector<CEnode*> vSortedByAddr;

    {
        LOCK(cs);

        CEnode* pprevEnode = NULL;
        CEnode* pverifiedEnode = NULL;

        BOOST_FOREACH(CEnode& mn, vEnodes) {
            vSortedByAddr.push_back(&mn);
        }

        sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());

        BOOST_FOREACH(CEnode* pmn, vSortedByAddr) {
            // check only (pre)enabled znodes
            if(!pmn->IsEnabled() && !pmn->IsPreEnabled()) continue;
            // initial step
            if(!pprevEnode) {
                pprevEnode = pmn;
                pverifiedEnode = pmn->IsPoSeVerified() ? pmn : NULL;
                continue;
            }
            // second+ step
            if(pmn->addr == pprevEnode->addr) {
                if(pverifiedEnode) {
                    // another znode with the same ip is verified, ban this one
                    vBan.push_back(pmn);
                } else if(pmn->IsPoSeVerified()) {
                    // this znode with the same ip is verified, ban previous one
                    vBan.push_back(pprevEnode);
                    // and keep a reference to be able to ban following znodes with the same ip
                    pverifiedEnode = pmn;
                }
            } else {
                pverifiedEnode = pmn->IsPoSeVerified() ? pmn : NULL;
            }
            pprevEnode = pmn;
        }
    }

    // ban duplicates
    BOOST_FOREACH(CEnode* pmn, vBan) {
        LogPrintf("CEnodeMan::CheckSameAddr -- increasing PoSe ban score for znode %s\n", pmn->vin.prevout.ToStringShort());
        pmn->IncreasePoSeBanScore();
    }
}

bool CEnodeMan::SendVerifyRequest(const CAddress& addr, const std::vector<CEnode*>& vSortedByAddr)
{
    if(netfulfilledman.HasFulfilledRequest(addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request")) {
        // we already asked for verification, not a good idea to do this too often, skip it
        LogPrint("znode", "CEnodeMan::SendVerifyRequest -- too many requests, skipping... addr=%s\n", addr.ToString());
        return false;
    }

    CNode* pnode = ConnectNode(addr, NULL, false, true);
    if(pnode == NULL) {
        LogPrintf("CEnodeMan::SendVerifyRequest -- can't connect to node to verify it, addr=%s\n", addr.ToString());
        return false;
    }

    netfulfilledman.AddFulfilledRequest(addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request");
    // use random nonce, store it and require node to reply with correct one later
    CEnodeVerification mnv(addr, GetRandInt(999999), pCurrentBlockIndex->nHeight - 1);
    mWeAskedForVerification[addr] = mnv;
    LogPrintf("CEnodeMan::SendVerifyRequest -- verifying node using nonce %d addr=%s\n", mnv.nonce, addr.ToString());
    pnode->PushMessage(NetMsgType::MNVERIFY, mnv);

    return true;
}

void CEnodeMan::SendVerifyReply(CNode* pnode, CEnodeVerification& mnv)
{
    // only znodes can sign this, why would someone ask regular node?
    if(!fZNode) {
        // do not ban, malicious node might be using my IP
        // and trying to confuse the node which tries to verify it
        return;
    }

    if(netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-reply")) {
//        // peer should not ask us that often
        LogPrintf("EnodeMan::SendVerifyReply -- ERROR: peer already asked me recently, peer=%d\n", pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        LogPrintf("EnodeMan::SendVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->id);
        return;
    }

    std::string strMessage = strprintf("%s%d%s", activeEnode.service.ToString(), mnv.nonce, blockHash.ToString());

    if(!darkSendSigner.SignMessage(strMessage, mnv.vchSig1, activeEnode.keyEnode)) {
        LogPrintf("EnodeMan::SendVerifyReply -- SignMessage() failed\n");
        return;
    }

    std::string strError;

    if(!darkSendSigner.VerifyMessage(activeEnode.pubKeyEnode, mnv.vchSig1, strMessage, strError)) {
        LogPrintf("EnodeMan::SendVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
        return;
    }

    pnode->PushMessage(NetMsgType::MNVERIFY, mnv);
    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-reply");
}

void CEnodeMan::ProcessVerifyReply(CNode* pnode, CEnodeVerification& mnv)
{
    std::string strError;

    // did we even ask for it? if that's the case we should have matching fulfilled request
    if(!netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request")) {
        LogPrintf("CEnodeMan::ProcessVerifyReply -- ERROR: we didn't ask for verification of %s, peer=%d\n", pnode->addr.ToString(), pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    // Received nonce for a known address must match the one we sent
    if(mWeAskedForVerification[pnode->addr].nonce != mnv.nonce) {
        LogPrintf("CEnodeMan::ProcessVerifyReply -- ERROR: wrong nounce: requested=%d, received=%d, peer=%d\n",
                    mWeAskedForVerification[pnode->addr].nonce, mnv.nonce, pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    // Received nBlockHeight for a known address must match the one we sent
    if(mWeAskedForVerification[pnode->addr].nBlockHeight != mnv.nBlockHeight) {
        LogPrintf("CEnodeMan::ProcessVerifyReply -- ERROR: wrong nBlockHeight: requested=%d, received=%d, peer=%d\n",
                    mWeAskedForVerification[pnode->addr].nBlockHeight, mnv.nBlockHeight, pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("EnodeMan::ProcessVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->id);
        return;
    }

//    // we already verified this address, why node is spamming?
    if(netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-done")) {
        LogPrintf("CEnodeMan::ProcessVerifyReply -- ERROR: already verified %s recently\n", pnode->addr.ToString());
        Misbehaving(pnode->id, 20);
        return;
    }

    {
        LOCK(cs);

        CEnode* prealEnode = NULL;
        std::vector<CEnode*> vpEnodesToBan;
        std::vector<CEnode>::iterator it = vEnodes.begin();
        std::string strMessage1 = strprintf("%s%d%s", pnode->addr.ToString(), mnv.nonce, blockHash.ToString());
        while(it != vEnodes.end()) {
            if(CAddress(it->addr, NODE_NETWORK) == pnode->addr) {
                if(darkSendSigner.VerifyMessage(it->pubKeyEnode, mnv.vchSig1, strMessage1, strError)) {
                    // found it!
                    prealEnode = &(*it);
                    if(!it->IsPoSeVerified()) {
                        it->DecreasePoSeBanScore();
                    }
                    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-done");

                    // we can only broadcast it if we are an activated znode
                    if(activeEnode.vin == CTxIn()) continue;
                    // update ...
                    mnv.addr = it->addr;
                    mnv.vin1 = it->vin;
                    mnv.vin2 = activeEnode.vin;
                    std::string strMessage2 = strprintf("%s%d%s%s%s", mnv.addr.ToString(), mnv.nonce, blockHash.ToString(),
                                            mnv.vin1.prevout.ToStringShort(), mnv.vin2.prevout.ToStringShort());
                    // ... and sign it
                    if(!darkSendSigner.SignMessage(strMessage2, mnv.vchSig2, activeEnode.keyEnode)) {
                        LogPrintf("EnodeMan::ProcessVerifyReply -- SignMessage() failed\n");
                        return;
                    }

                    std::string strError;

                    if(!darkSendSigner.VerifyMessage(activeEnode.pubKeyEnode, mnv.vchSig2, strMessage2, strError)) {
                        LogPrintf("EnodeMan::ProcessVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
                        return;
                    }

                    mWeAskedForVerification[pnode->addr] = mnv;
                    mnv.Relay();

                } else {
                    vpEnodesToBan.push_back(&(*it));
                }
            }
            ++it;
        }
        // no real znode found?...
        if(!prealEnode) {
            // this should never be the case normally,
            // only if someone is trying to game the system in some way or smth like that
            LogPrintf("CEnodeMan::ProcessVerifyReply -- ERROR: no real znode found for addr %s\n", pnode->addr.ToString());
            Misbehaving(pnode->id, 20);
            return;
        }
        LogPrintf("CEnodeMan::ProcessVerifyReply -- verified real znode %s for addr %s\n",
                    prealEnode->vin.prevout.ToStringShort(), pnode->addr.ToString());
        // increase ban score for everyone else
        BOOST_FOREACH(CEnode* pmn, vpEnodesToBan) {
            pmn->IncreasePoSeBanScore();
            LogPrint("znode", "CEnodeMan::ProcessVerifyBroadcast -- increased PoSe ban score for %s addr %s, new score %d\n",
                        prealEnode->vin.prevout.ToStringShort(), pnode->addr.ToString(), pmn->nPoSeBanScore);
        }
        LogPrintf("CEnodeMan::ProcessVerifyBroadcast -- PoSe score increased for %d fake znodes, addr %s\n",
                    (int)vpEnodesToBan.size(), pnode->addr.ToString());
    }
}

void CEnodeMan::ProcessVerifyBroadcast(CNode* pnode, const CEnodeVerification& mnv)
{
    std::string strError;

    if(mapSeenEnodeVerification.find(mnv.GetHash()) != mapSeenEnodeVerification.end()) {
        // we already have one
        return;
    }
    mapSeenEnodeVerification[mnv.GetHash()] = mnv;

    // we don't care about history
    if(mnv.nBlockHeight < pCurrentBlockIndex->nHeight - MAX_POSE_BLOCKS) {
        LogPrint("znode", "EnodeMan::ProcessVerifyBroadcast -- Outdated: current block %d, verification block %d, peer=%d\n",
                    pCurrentBlockIndex->nHeight, mnv.nBlockHeight, pnode->id);
        return;
    }

    if(mnv.vin1.prevout == mnv.vin2.prevout) {
        LogPrint("znode", "EnodeMan::ProcessVerifyBroadcast -- ERROR: same vins %s, peer=%d\n",
                    mnv.vin1.prevout.ToStringShort(), pnode->id);
        // that was NOT a good idea to cheat and verify itself,
        // ban the node we received such message from
        Misbehaving(pnode->id, 100);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("EnodeMan::ProcessVerifyBroadcast -- Can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->id);
        return;
    }

    int nRank = GetEnodeRank(mnv.vin2, mnv.nBlockHeight, MIN_POSE_PROTO_VERSION);

    if (nRank == -1) {
        LogPrint("znode", "CEnodeMan::ProcessVerifyBroadcast -- Can't calculate rank for znode %s\n",
                    mnv.vin2.prevout.ToStringShort());
        return;
    }

    if(nRank > MAX_POSE_RANK) {
        LogPrint("znode", "CEnodeMan::ProcessVerifyBroadcast -- Mastrernode %s is not in top %d, current rank %d, peer=%d\n",
                    mnv.vin2.prevout.ToStringShort(), (int)MAX_POSE_RANK, nRank, pnode->id);
        return;
    }

    {
        LOCK(cs);

        std::string strMessage1 = strprintf("%s%d%s", mnv.addr.ToString(), mnv.nonce, blockHash.ToString());
        std::string strMessage2 = strprintf("%s%d%s%s%s", mnv.addr.ToString(), mnv.nonce, blockHash.ToString(),
                                mnv.vin1.prevout.ToStringShort(), mnv.vin2.prevout.ToStringShort());

        CEnode* pmn1 = Find(mnv.vin1);
        if(!pmn1) {
            LogPrintf("CEnodeMan::ProcessVerifyBroadcast -- can't find znode1 %s\n", mnv.vin1.prevout.ToStringShort());
            return;
        }

        CEnode* pmn2 = Find(mnv.vin2);
        if(!pmn2) {
            LogPrintf("CEnodeMan::ProcessVerifyBroadcast -- can't find znode2 %s\n", mnv.vin2.prevout.ToStringShort());
            return;
        }

        if(pmn1->addr != mnv.addr) {
            LogPrintf("CEnodeMan::ProcessVerifyBroadcast -- addr %s do not match %s\n", mnv.addr.ToString(), pnode->addr.ToString());
            return;
        }

        if(darkSendSigner.VerifyMessage(pmn1->pubKeyEnode, mnv.vchSig1, strMessage1, strError)) {
            LogPrintf("EnodeMan::ProcessVerifyBroadcast -- VerifyMessage() for znode1 failed, error: %s\n", strError);
            return;
        }

        if(darkSendSigner.VerifyMessage(pmn2->pubKeyEnode, mnv.vchSig2, strMessage2, strError)) {
            LogPrintf("EnodeMan::ProcessVerifyBroadcast -- VerifyMessage() for znode2 failed, error: %s\n", strError);
            return;
        }

        if(!pmn1->IsPoSeVerified()) {
            pmn1->DecreasePoSeBanScore();
        }
        mnv.Relay();

        LogPrintf("CEnodeMan::ProcessVerifyBroadcast -- verified znode %s for addr %s\n",
                    pmn1->vin.prevout.ToStringShort(), pnode->addr.ToString());

        // increase ban score for everyone else with the same addr
        int nCount = 0;
        BOOST_FOREACH(CEnode& mn, vEnodes) {
            if(mn.addr != mnv.addr || mn.vin.prevout == mnv.vin1.prevout) continue;
            mn.IncreasePoSeBanScore();
            nCount++;
            LogPrint("znode", "CEnodeMan::ProcessVerifyBroadcast -- increased PoSe ban score for %s addr %s, new score %d\n",
                        mn.vin.prevout.ToStringShort(), mn.addr.ToString(), mn.nPoSeBanScore);
        }
        LogPrintf("CEnodeMan::ProcessVerifyBroadcast -- PoSe score incresed for %d fake znodes, addr %s\n",
                    nCount, pnode->addr.ToString());
    }
}

std::string CEnodeMan::ToString() const
{
    std::ostringstream info;

    info << "Enodes: " << (int)vEnodes.size() <<
            ", peers who asked us for Enode list: " << (int)mAskedUsForEnodeList.size() <<
            ", peers we asked for Enode list: " << (int)mWeAskedForEnodeList.size() <<
            ", entries in Enode list we asked for: " << (int)mWeAskedForEnodeListEntry.size() <<
            ", znode index size: " << indexEnodes.GetSize() <<
            ", nDsqCount: " << (int)nDsqCount;

    return info.str();
}

void CEnodeMan::UpdateEnodeList(CEnodeBroadcast mnb)
{
    try {
        LogPrintf("CEnodeMan::UpdateEnodeList\n");
        LOCK2(cs_main, cs);
        mapSeenEnodePing.insert(std::make_pair(mnb.lastPing.GetHash(), mnb.lastPing));
        mapSeenEnodeBroadcast.insert(std::make_pair(mnb.GetHash(), std::make_pair(GetTime(), mnb)));

        LogPrintf("CEnodeMan::UpdateEnodeList -- znode=%s  addr=%s\n", mnb.vin.prevout.ToStringShort(), mnb.addr.ToString());

        CEnode *pmn = Find(mnb.vin);
        if (pmn == NULL) {
            CEnode mn(mnb);
            if (Add(mn)) {
                znodeSync.AddedEnodeList();
            }
        } else {
            CEnodeBroadcast mnbOld = mapSeenEnodeBroadcast[CEnodeBroadcast(*pmn).GetHash()].second;
            if (pmn->UpdateFromNewBroadcast(mnb)) {
                znodeSync.AddedEnodeList();
                mapSeenEnodeBroadcast.erase(mnbOld.GetHash());
            }
        }
    } catch (const std::exception &e) {
        PrintExceptionContinue(&e, "UpdateEnodeList");
    }
}

bool CEnodeMan::CheckMnbAndUpdateEnodeList(CNode* pfrom, CEnodeBroadcast mnb, int& nDos)
{
    // Need LOCK2 here to ensure consistent locking order because the SimpleCheck call below locks cs_main
    LOCK(cs_main);

    {
        LOCK(cs);
        nDos = 0;
        LogPrint("znode", "CEnodeMan::CheckMnbAndUpdateEnodeList -- znode=%s\n", mnb.vin.prevout.ToStringShort());

        uint256 hash = mnb.GetHash();
        if (mapSeenEnodeBroadcast.count(hash) && !mnb.fRecovery) { //seen
            LogPrint("znode", "CEnodeMan::CheckMnbAndUpdateEnodeList -- znode=%s seen\n", mnb.vin.prevout.ToStringShort());
            // less then 2 pings left before this MN goes into non-recoverable state, bump sync timeout
            if (GetTime() - mapSeenEnodeBroadcast[hash].first > ENODE_NEW_START_REQUIRED_SECONDS - ENODE_MIN_MNP_SECONDS * 2) {
                LogPrint("znode", "CEnodeMan::CheckMnbAndUpdateEnodeList -- znode=%s seen update\n", mnb.vin.prevout.ToStringShort());
                mapSeenEnodeBroadcast[hash].first = GetTime();
                znodeSync.AddedEnodeList();
            }
            // did we ask this node for it?
            if (pfrom && IsMnbRecoveryRequested(hash) && GetTime() < mMnbRecoveryRequests[hash].first) {
                LogPrint("znode", "CEnodeMan::CheckMnbAndUpdateEnodeList -- mnb=%s seen request\n", hash.ToString());
                if (mMnbRecoveryRequests[hash].second.count(pfrom->addr)) {
                    LogPrint("znode", "CEnodeMan::CheckMnbAndUpdateEnodeList -- mnb=%s seen request, addr=%s\n", hash.ToString(), pfrom->addr.ToString());
                    // do not allow node to send same mnb multiple times in recovery mode
                    mMnbRecoveryRequests[hash].second.erase(pfrom->addr);
                    // does it have newer lastPing?
                    if (mnb.lastPing.sigTime > mapSeenEnodeBroadcast[hash].second.lastPing.sigTime) {
                        // simulate Check
                        CEnode mnTemp = CEnode(mnb);
                        mnTemp.Check();
                        LogPrint("znode", "CEnodeMan::CheckMnbAndUpdateEnodeList -- mnb=%s seen request, addr=%s, better lastPing: %d min ago, projected mn state: %s\n", hash.ToString(), pfrom->addr.ToString(), (GetTime() - mnb.lastPing.sigTime) / 60, mnTemp.GetStateString());
                        if (mnTemp.IsValidStateForAutoStart(mnTemp.nActiveState)) {
                            // this node thinks it's a good one
                            LogPrint("znode", "CEnodeMan::CheckMnbAndUpdateEnodeList -- znode=%s seen good\n", mnb.vin.prevout.ToStringShort());
                            mMnbRecoveryGoodReplies[hash].push_back(mnb);
                        }
                    }
                }
            }
            return true;
        }
        mapSeenEnodeBroadcast.insert(std::make_pair(hash, std::make_pair(GetTime(), mnb)));

        LogPrint("znode", "CEnodeMan::CheckMnbAndUpdateEnodeList -- znode=%s new\n", mnb.vin.prevout.ToStringShort());

        if (!mnb.SimpleCheck(nDos)) {
            LogPrint("znode", "CEnodeMan::CheckMnbAndUpdateEnodeList -- SimpleCheck() failed, znode=%s\n", mnb.vin.prevout.ToStringShort());
            return false;
        }

        // search Enode list
        CEnode *pmn = Find(mnb.vin);
        if (pmn) {
            CEnodeBroadcast mnbOld = mapSeenEnodeBroadcast[CEnodeBroadcast(*pmn).GetHash()].second;
            if (!mnb.Update(pmn, nDos)) {
                LogPrint("znode", "CEnodeMan::CheckMnbAndUpdateEnodeList -- Update() failed, znode=%s\n", mnb.vin.prevout.ToStringShort());
                return false;
            }
            if (hash != mnbOld.GetHash()) {
                mapSeenEnodeBroadcast.erase(mnbOld.GetHash());
            }
        }
    } // end of LOCK(cs);

    if(mnb.CheckOutpoint(nDos)) {
        Add(mnb);
        znodeSync.AddedEnodeList();
        // if it matches our Enode privkey...
        if(fZNode && mnb.pubKeyEnode == activeEnode.pubKeyEnode) {
            mnb.nPoSeBanScore = -ENODE_POSE_BAN_MAX_SCORE;
            if(mnb.nProtocolVersion == PROTOCOL_VERSION) {
                // ... and PROTOCOL_VERSION, then we've been remotely activated ...
                LogPrintf("CEnodeMan::CheckMnbAndUpdateEnodeList -- Got NEW Enode entry: znode=%s  sigTime=%lld  addr=%s\n",
                            mnb.vin.prevout.ToStringShort(), mnb.sigTime, mnb.addr.ToString());
                activeEnode.ManageState();
            } else {
                // ... otherwise we need to reactivate our node, do not add it to the list and do not relay
                // but also do not ban the node we get this message from
                LogPrintf("CEnodeMan::CheckMnbAndUpdateEnodeList -- wrong PROTOCOL_VERSION, re-activate your MN: message nProtocolVersion=%d  PROTOCOL_VERSION=%d\n", mnb.nProtocolVersion, PROTOCOL_VERSION);
                return false;
            }
        }
        mnb.RelayZNode();
    } else {
        LogPrintf("CEnodeMan::CheckMnbAndUpdateEnodeList -- Rejected Enode entry: %s  addr=%s\n", mnb.vin.prevout.ToStringShort(), mnb.addr.ToString());
        return false;
    }

    return true;
}

void CEnodeMan::UpdateLastPaid()
{
    LOCK(cs);
    if(fLiteMode) return;
    if(!pCurrentBlockIndex) {
        // LogPrintf("CEnodeMan::UpdateLastPaid, pCurrentBlockIndex=NULL\n");
        return;
    }

    static bool IsFirstRun = true;
    // Do full scan on first run or if we are not a znode
    // (MNs should update this info on every block, so limited scan should be enough for them)
    int nMaxBlocksToScanBack = (IsFirstRun || !fZNode) ? mnpayments.GetStorageLimit() : LAST_PAID_SCAN_BLOCKS;

    LogPrint("mnpayments", "CEnodeMan::UpdateLastPaid -- nHeight=%d, nMaxBlocksToScanBack=%d, IsFirstRun=%s\n",
                             pCurrentBlockIndex->nHeight, nMaxBlocksToScanBack, IsFirstRun ? "true" : "false");

    BOOST_FOREACH(CEnode& mn, vEnodes) {
        mn.UpdateLastPaid(pCurrentBlockIndex, nMaxBlocksToScanBack);
    }

    // every time is like the first time if winners list is not synced
    IsFirstRun = !znodeSync.IsWinnersListSynced();
}

void CEnodeMan::CheckAndRebuildEnodeIndex()
{
    LOCK(cs);

    if(GetTime() - nLastIndexRebuildTime < MIN_INDEX_REBUILD_TIME) {
        return;
    }

    if(indexEnodes.GetSize() <= MAX_EXPECTED_INDEX_SIZE) {
        return;
    }

    if(indexEnodes.GetSize() <= int(vEnodes.size())) {
        return;
    }

    indexEnodesOld = indexEnodes;
    indexEnodes.Clear();
    for(size_t i = 0; i < vEnodes.size(); ++i) {
        indexEnodes.AddEnodeVIN(vEnodes[i].vin);
    }

    fIndexRebuilt = true;
    nLastIndexRebuildTime = GetTime();
}

void CEnodeMan::UpdateWatchdogVoteTime(const CTxIn& vin)
{
    LOCK(cs);
    CEnode* pMN = Find(vin);
    if(!pMN)  {
        return;
    }
    pMN->UpdateWatchdogVoteTime();
    nLastWatchdogVoteTime = GetTime();
}

bool CEnodeMan::IsWatchdogActive()
{
    LOCK(cs);
    // Check if any znodes have voted recently, otherwise return false
    return (GetTime() - nLastWatchdogVoteTime) <= ENODE_WATCHDOG_MAX_SECONDS;
}

void CEnodeMan::CheckEnode(const CTxIn& vin, bool fForce)
{
    LOCK(cs);
    CEnode* pMN = Find(vin);
    if(!pMN)  {
        return;
    }
    pMN->Check(fForce);
}

void CEnodeMan::CheckEnode(const CPubKey& pubKeyEnode, bool fForce)
{
    LOCK(cs);
    CEnode* pMN = Find(pubKeyEnode);
    if(!pMN)  {
        return;
    }
    pMN->Check(fForce);
}

int CEnodeMan::GetEnodeState(const CTxIn& vin)
{
    LOCK(cs);
    CEnode* pMN = Find(vin);
    if(!pMN)  {
        return CEnode::ENODE_NEW_START_REQUIRED;
    }
    return pMN->nActiveState;
}

int CEnodeMan::GetEnodeState(const CPubKey& pubKeyEnode)
{
    LOCK(cs);
    CEnode* pMN = Find(pubKeyEnode);
    if(!pMN)  {
        return CEnode::ENODE_NEW_START_REQUIRED;
    }
    return pMN->nActiveState;
}

bool CEnodeMan::IsEnodePingedWithin(const CTxIn& vin, int nSeconds, int64_t nTimeToCheckAt)
{
    LOCK(cs);
    CEnode* pMN = Find(vin);
    if(!pMN) {
        return false;
    }
    return pMN->IsPingedWithin(nSeconds, nTimeToCheckAt);
}

void CEnodeMan::SetEnodeLastPing(const CTxIn& vin, const CEnodePing& mnp)
{
    LOCK(cs);
    CEnode* pMN = Find(vin);
    if(!pMN)  {
        return;
    }
    pMN->lastPing = mnp;
    mapSeenEnodePing.insert(std::make_pair(mnp.GetHash(), mnp));

    CEnodeBroadcast mnb(*pMN);
    uint256 hash = mnb.GetHash();
    if(mapSeenEnodeBroadcast.count(hash)) {
        mapSeenEnodeBroadcast[hash].second.lastPing = mnp;
    }
}

void CEnodeMan::UpdatedBlockTip(const CBlockIndex *pindex)
{
    pCurrentBlockIndex = pindex;
    LogPrint("znode", "CEnodeMan::UpdatedBlockTip -- pCurrentBlockIndex->nHeight=%d\n", pCurrentBlockIndex->nHeight);

    CheckSameAddr();

    if(fZNode) {
        // normal wallet does not need to update this every block, doing update on rpc call should be enough
        UpdateLastPaid();
    }
}

void CEnodeMan::NotifyEnodeUpdates()
{
    // Avoid double locking
    bool fEnodesAddedLocal = false;
    bool fEnodesRemovedLocal = false;
    {
        LOCK(cs);
        fEnodesAddedLocal = fEnodesAdded;
        fEnodesRemovedLocal = fEnodesRemoved;
    }

    if(fEnodesAddedLocal) {
//        governance.CheckEnodeOrphanObjects();
//        governance.CheckEnodeOrphanVotes();
    }
    if(fEnodesRemovedLocal) {
//        governance.UpdateCachesAndClean();
    }

    LOCK(cs);
    fEnodesAdded = false;
    fEnodesRemoved = false;
}
