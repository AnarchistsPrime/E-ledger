// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ENODEMAN_H
#define ENODEMAN_H

#include "znode.h"
#include "sync.h"

using namespace std;

class CEnodeMan;

extern CEnodeMan mnodeman;

/**
 * Provides a forward and reverse index between MN vin's and integers.
 *
 * This mapping is normally add-only and is expected to be permanent
 * It is only rebuilt if the size of the index exceeds the expected maximum number
 * of MN's and the current number of known MN's.
 *
 * The external interface to this index is provided via delegation by CEnodeMan
 */
class CEnodeIndex
{
public: // Types
    typedef std::map<CTxIn,int> index_m_t;

    typedef index_m_t::iterator index_m_it;

    typedef index_m_t::const_iterator index_m_cit;

    typedef std::map<int,CTxIn> rindex_m_t;

    typedef rindex_m_t::iterator rindex_m_it;

    typedef rindex_m_t::const_iterator rindex_m_cit;

private:
    int                  nSize;

    index_m_t            mapIndex;

    rindex_m_t           mapReverseIndex;

public:
    CEnodeIndex();

    int GetSize() const {
        return nSize;
    }

    /// Retrieve znode vin by index
    bool Get(int nIndex, CTxIn& vinEnode) const;

    /// Get index of a znode vin
    int GetEnodeIndex(const CTxIn& vinEnode) const;

    void AddEnodeVIN(const CTxIn& vinEnode);

    void Clear();

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(mapIndex);
        if(ser_action.ForRead()) {
            RebuildIndex();
        }
    }

private:
    void RebuildIndex();

};

class CEnodeMan
{
public:
    typedef std::map<CTxIn,int> index_m_t;

    typedef index_m_t::iterator index_m_it;

    typedef index_m_t::const_iterator index_m_cit;

private:
    static const int MAX_EXPECTED_INDEX_SIZE = 30000;

    /// Only allow 1 index rebuild per hour
    static const int64_t MIN_INDEX_REBUILD_TIME = 3600;

    static const std::string SERIALIZATION_VERSION_STRING;

    static const int DSEG_UPDATE_SECONDS        = 3 * 60 * 60;

    static const int LAST_PAID_SCAN_BLOCKS      = 100;

    static const int MIN_POSE_PROTO_VERSION     = 70203;
    static const int MAX_POSE_CONNECTIONS       = 10;
    static const int MAX_POSE_RANK              = 10;
    static const int MAX_POSE_BLOCKS            = 10;

    static const int MNB_RECOVERY_QUORUM_TOTAL      = 10;
    static const int MNB_RECOVERY_QUORUM_REQUIRED   = 6;
    static const int MNB_RECOVERY_MAX_ASK_ENTRIES   = 10;
    static const int MNB_RECOVERY_WAIT_SECONDS      = 60;
    static const int MNB_RECOVERY_RETRY_SECONDS     = 3 * 60 * 60;


    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

    // Keep track of current block index
    const CBlockIndex *pCurrentBlockIndex;

    // map to hold all MNs
    std::vector<CEnode> vEnodes;
    // who's asked for the Enode list and the last time
    std::map<CNetAddr, int64_t> mAskedUsForEnodeList;
    // who we asked for the Enode list and the last time
    std::map<CNetAddr, int64_t> mWeAskedForEnodeList;
    // which Enodes we've asked for
    std::map<COutPoint, std::map<CNetAddr, int64_t> > mWeAskedForEnodeListEntry;
    // who we asked for the znode verification
    std::map<CNetAddr, CEnodeVerification> mWeAskedForVerification;

    // these maps are used for znode recovery from ENODE_NEW_START_REQUIRED state
    std::map<uint256, std::pair< int64_t, std::set<CNetAddr> > > mMnbRecoveryRequests;
    std::map<uint256, std::vector<CEnodeBroadcast> > mMnbRecoveryGoodReplies;
    std::list< std::pair<CService, uint256> > listScheduledMnbRequestConnections;

    int64_t nLastIndexRebuildTime;

    CEnodeIndex indexEnodes;

    CEnodeIndex indexEnodesOld;

    /// Set when index has been rebuilt, clear when read
    bool fIndexRebuilt;

    /// Set when znodes are added, cleared when CGovernanceManager is notified
    bool fEnodesAdded;

    /// Set when znodes are removed, cleared when CGovernanceManager is notified
    bool fEnodesRemoved;

    std::vector<uint256> vecDirtyGovernanceObjectHashes;

    int64_t nLastWatchdogVoteTime;

    friend class CEnodeSync;

public:
    // Keep track of all broadcasts I've seen
    std::map<uint256, std::pair<int64_t, CEnodeBroadcast> > mapSeenEnodeBroadcast;
    // Keep track of all pings I've seen
    std::map<uint256, CEnodePing> mapSeenEnodePing;
    // Keep track of all verifications I've seen
    std::map<uint256, CEnodeVerification> mapSeenEnodeVerification;
    // keep track of dsq count to prevent znodes from gaming darksend queue
    int64_t nDsqCount;


    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        LOCK(cs);
        std::string strVersion;
        if(ser_action.ForRead()) {
            READWRITE(strVersion);
        }
        else {
            strVersion = SERIALIZATION_VERSION_STRING; 
            READWRITE(strVersion);
        }

        READWRITE(vEnodes);
        READWRITE(mAskedUsForEnodeList);
        READWRITE(mWeAskedForEnodeList);
        READWRITE(mWeAskedForEnodeListEntry);
        READWRITE(mMnbRecoveryRequests);
        READWRITE(mMnbRecoveryGoodReplies);
        READWRITE(nLastWatchdogVoteTime);
        READWRITE(nDsqCount);

        READWRITE(mapSeenEnodeBroadcast);
        READWRITE(mapSeenEnodePing);
        READWRITE(indexEnodes);
        if(ser_action.ForRead() && (strVersion != SERIALIZATION_VERSION_STRING)) {
            Clear();
        }
    }

    CEnodeMan();

    /// Add an entry
    bool Add(CEnode &mn);

    /// Ask (source) node for mnb
    void AskForMN(CNode *pnode, const CTxIn &vin);
    void AskForMnb(CNode *pnode, const uint256 &hash);

    /// Check all Enodes
    void Check();

    /// Check all Enodes and remove inactive
    void CheckAndRemove();

    /// Clear Enode vector
    void Clear();

    /// Count Enodes filtered by nProtocolVersion.
    /// Enode nProtocolVersion should match or be above the one specified in param here.
    int CountEnodes(int nProtocolVersion = -1);
    /// Count enabled Enodes filtered by nProtocolVersion.
    /// Enode nProtocolVersion should match or be above the one specified in param here.
    int CountEnabled(int nProtocolVersion = -1);

    /// Count Enodes by network type - NET_IPV4, NET_IPV6, NET_TOR
    // int CountByIP(int nNetworkType);

    void DsegUpdate(CNode* pnode);

    /// Find an entry
    CEnode* Find(const CScript &payee);
    CEnode* Find(const CTxIn& vin);
    CEnode* Find(const CPubKey& pubKeyEnode);

    /// Versions of Find that are safe to use from outside the class
    bool Get(const CPubKey& pubKeyEnode, CEnode& znode);
    bool Get(const CTxIn& vin, CEnode& znode);

    /// Retrieve znode vin by index
    bool Get(int nIndex, CTxIn& vinEnode, bool& fIndexRebuiltOut) {
        LOCK(cs);
        fIndexRebuiltOut = fIndexRebuilt;
        return indexEnodes.Get(nIndex, vinEnode);
    }

    bool GetIndexRebuiltFlag() {
        LOCK(cs);
        return fIndexRebuilt;
    }

    /// Get index of a znode vin
    int GetEnodeIndex(const CTxIn& vinEnode) {
        LOCK(cs);
        return indexEnodes.GetEnodeIndex(vinEnode);
    }

    /// Get old index of a znode vin
    int GetEnodeIndexOld(const CTxIn& vinEnode) {
        LOCK(cs);
        return indexEnodesOld.GetEnodeIndex(vinEnode);
    }

    /// Get znode VIN for an old index value
    bool GetEnodeVinForIndexOld(int nEnodeIndex, CTxIn& vinEnodeOut) {
        LOCK(cs);
        return indexEnodesOld.Get(nEnodeIndex, vinEnodeOut);
    }

    /// Get index of a znode vin, returning rebuild flag
    int GetEnodeIndex(const CTxIn& vinEnode, bool& fIndexRebuiltOut) {
        LOCK(cs);
        fIndexRebuiltOut = fIndexRebuilt;
        return indexEnodes.GetEnodeIndex(vinEnode);
    }

    void ClearOldEnodeIndex() {
        LOCK(cs);
        indexEnodesOld.Clear();
        fIndexRebuilt = false;
    }

    bool Has(const CTxIn& vin);

    znode_info_t GetEnodeInfo(const CTxIn& vin);

    znode_info_t GetEnodeInfo(const CPubKey& pubKeyEnode);

    char* GetNotQualifyReason(CEnode& mn, int nBlockHeight, bool fFilterSigTime, int nMnCount);

    /// Find an entry in the znode list that is next to be paid
    CEnode* GetNextEnodeInQueueForPayment(int nBlockHeight, bool fFilterSigTime, int& nCount);
    /// Same as above but use current block height
    CEnode* GetNextEnodeInQueueForPayment(bool fFilterSigTime, int& nCount);

    /// Find a random entry
    CEnode* FindRandomNotInVec(const std::vector<CTxIn> &vecToExclude, int nProtocolVersion = -1);

    std::vector<CEnode> GetFullEnodeVector() { return vEnodes; }

    std::vector<std::pair<int, CEnode> > GetEnodeRanks(int nBlockHeight = -1, int nMinProtocol=0);
    int GetEnodeRank(const CTxIn &vin, int nBlockHeight, int nMinProtocol=0, bool fOnlyActive=true);
    CEnode* GetEnodeByRank(int nRank, int nBlockHeight, int nMinProtocol=0, bool fOnlyActive=true);

    void ProcessEnodeConnections();
    std::pair<CService, std::set<uint256> > PopScheduledMnbRequestConnection();

    void ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);

    void DoFullVerificationStep();
    void CheckSameAddr();
    bool SendVerifyRequest(const CAddress& addr, const std::vector<CEnode*>& vSortedByAddr);
    void SendVerifyReply(CNode* pnode, CEnodeVerification& mnv);
    void ProcessVerifyReply(CNode* pnode, CEnodeVerification& mnv);
    void ProcessVerifyBroadcast(CNode* pnode, const CEnodeVerification& mnv);

    /// Return the number of (unique) Enodes
    int size() { return vEnodes.size(); }

    std::string ToString() const;

    /// Update znode list and maps using provided CEnodeBroadcast
    void UpdateEnodeList(CEnodeBroadcast mnb);
    /// Perform complete check and only then update list and maps
    bool CheckMnbAndUpdateEnodeList(CNode* pfrom, CEnodeBroadcast mnb, int& nDos);
    bool IsMnbRecoveryRequested(const uint256& hash) { return mMnbRecoveryRequests.count(hash); }

    void UpdateLastPaid();

    void CheckAndRebuildEnodeIndex();

    void AddDirtyGovernanceObjectHash(const uint256& nHash)
    {
        LOCK(cs);
        vecDirtyGovernanceObjectHashes.push_back(nHash);
    }

    std::vector<uint256> GetAndClearDirtyGovernanceObjectHashes()
    {
        LOCK(cs);
        std::vector<uint256> vecTmp = vecDirtyGovernanceObjectHashes;
        vecDirtyGovernanceObjectHashes.clear();
        return vecTmp;;
    }

    bool IsWatchdogActive();
    void UpdateWatchdogVoteTime(const CTxIn& vin);
    bool AddGovernanceVote(const CTxIn& vin, uint256 nGovernanceObjectHash);
    void RemoveGovernanceObject(uint256 nGovernanceObjectHash);

    void CheckEnode(const CTxIn& vin, bool fForce = false);
    void CheckEnode(const CPubKey& pubKeyEnode, bool fForce = false);

    int GetEnodeState(const CTxIn& vin);
    int GetEnodeState(const CPubKey& pubKeyEnode);

    bool IsEnodePingedWithin(const CTxIn& vin, int nSeconds, int64_t nTimeToCheckAt = -1);
    void SetEnodeLastPing(const CTxIn& vin, const CEnodePing& mnp);

    void UpdatedBlockTip(const CBlockIndex *pindex);

    /**
     * Called to notify CGovernanceManager that the znode index has been updated.
     * Must be called while not holding the CEnodeMan::cs mutex
     */
    void NotifyEnodeUpdates();

};

#endif
