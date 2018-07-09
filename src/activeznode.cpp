// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activeznode.h"
#include "znode.h"
#include "znode-sync.h"
#include "znodeman.h"
#include "protocol.h"

extern CWallet *pwalletMain;

// Keep track of the active Enode
CActiveEnode activeEnode;

void CActiveEnode::ManageState() {
    LogPrint("znode", "CActiveEnode::ManageState -- Start\n");
    if (!fZNode) {
        LogPrint("znode", "CActiveEnode::ManageState -- Not a znode, returning\n");
        return;
    }

    if (Params().NetworkIDString() != CBaseChainParams::REGTEST && !znodeSync.IsBlockchainSynced()) {
        nState = ACTIVE_ENODE_SYNC_IN_PROCESS;
        LogPrintf("CActiveEnode::ManageState -- %s: %s\n", GetStateString(), GetStatus());
        return;
    }

    if (nState == ACTIVE_ENODE_SYNC_IN_PROCESS) {
        nState = ACTIVE_ENODE_INITIAL;
    }

    LogPrint("znode", "CActiveEnode::ManageState -- status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);

    if (eType == ENODE_UNKNOWN) {
        ManageStateInitial();
    }

    if (eType == ENODE_REMOTE) {
        ManageStateRemote();
    } else if (eType == ENODE_LOCAL) {
        // Try Remote Start first so the started local znode can be restarted without recreate znode broadcast.
        ManageStateRemote();
        if (nState != ACTIVE_ENODE_STARTED)
            ManageStateLocal();
    }

    SendEnodePing();
}

std::string CActiveEnode::GetStateString() const {
    switch (nState) {
        case ACTIVE_ENODE_INITIAL:
            return "INITIAL";
        case ACTIVE_ENODE_SYNC_IN_PROCESS:
            return "SYNC_IN_PROCESS";
        case ACTIVE_ENODE_INPUT_TOO_NEW:
            return "INPUT_TOO_NEW";
        case ACTIVE_ENODE_NOT_CAPABLE:
            return "NOT_CAPABLE";
        case ACTIVE_ENODE_STARTED:
            return "STARTED";
        default:
            return "UNKNOWN";
    }
}

std::string CActiveEnode::GetStatus() const {
    switch (nState) {
        case ACTIVE_ENODE_INITIAL:
            return "Node just started, not yet activated";
        case ACTIVE_ENODE_SYNC_IN_PROCESS:
            return "Sync in progress. Must wait until sync is complete to start Enode";
        case ACTIVE_ENODE_INPUT_TOO_NEW:
            return strprintf("Enode input must have at least %d confirmations",
                             Params().GetConsensus().nEnodeMinimumConfirmations);
        case ACTIVE_ENODE_NOT_CAPABLE:
            return "Not capable znode: " + strNotCapableReason;
        case ACTIVE_ENODE_STARTED:
            return "Enode successfully started";
        default:
            return "Unknown";
    }
}

std::string CActiveEnode::GetTypeString() const {
    std::string strType;
    switch (eType) {
        case ENODE_UNKNOWN:
            strType = "UNKNOWN";
            break;
        case ENODE_REMOTE:
            strType = "REMOTE";
            break;
        case ENODE_LOCAL:
            strType = "LOCAL";
            break;
        default:
            strType = "UNKNOWN";
            break;
    }
    return strType;
}

bool CActiveEnode::SendEnodePing() {
    if (!fPingerEnabled) {
        LogPrint("znode",
                 "CActiveEnode::SendEnodePing -- %s: znode ping service is disabled, skipping...\n",
                 GetStateString());
        return false;
    }

    if (!mnodeman.Has(vin)) {
        strNotCapableReason = "Enode not in znode list";
        nState = ACTIVE_ENODE_NOT_CAPABLE;
        LogPrintf("CActiveEnode::SendEnodePing -- %s: %s\n", GetStateString(), strNotCapableReason);
        return false;
    }

    CEnodePing mnp(vin);
    if (!mnp.Sign(keyEnode, pubKeyEnode)) {
        LogPrintf("CActiveEnode::SendEnodePing -- ERROR: Couldn't sign Enode Ping\n");
        return false;
    }

    // Update lastPing for our znode in Enode list
    if (mnodeman.IsEnodePingedWithin(vin, ENODE_MIN_MNP_SECONDS, mnp.sigTime)) {
        LogPrintf("CActiveEnode::SendEnodePing -- Too early to send Enode Ping\n");
        return false;
    }

    mnodeman.SetEnodeLastPing(vin, mnp);

    LogPrintf("CActiveEnode::SendEnodePing -- Relaying ping, collateral=%s\n", vin.ToString());
    mnp.Relay();

    return true;
}

void CActiveEnode::ManageStateInitial() {
    LogPrint("znode", "CActiveEnode::ManageStateInitial -- status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);

    // Check that our local network configuration is correct
    if (!fListen) {
        // listen option is probably overwritten by smth else, no good
        nState = ACTIVE_ENODE_NOT_CAPABLE;
        strNotCapableReason = "Enode must accept connections from outside. Make sure listen configuration option is not overwritten by some another parameter.";
        LogPrintf("CActiveEnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    bool fFoundLocal = false;
    {
        LOCK(cs_vNodes);

        // First try to find whatever local address is specified by externalip option
        fFoundLocal = GetLocal(service) && CEnode::IsValidNetAddr(service);
        if (!fFoundLocal) {
            // nothing and no live connections, can't do anything for now
            if (vNodes.empty()) {
                nState = ACTIVE_ENODE_NOT_CAPABLE;
                strNotCapableReason = "Can't detect valid external address. Will retry when there are some connections available.";
                LogPrintf("CActiveEnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
                return;
            }
            // We have some peers, let's try to find our local address from one of them
            BOOST_FOREACH(CNode * pnode, vNodes)
            {
                if (pnode->fSuccessfullyConnected && pnode->addr.IsIPv4()) {
                    fFoundLocal = GetLocal(service, &pnode->addr) && CEnode::IsValidNetAddr(service);
                    if (fFoundLocal) break;
                }
            }
        }
    }

    if (!fFoundLocal) {
        nState = ACTIVE_ENODE_NOT_CAPABLE;
        strNotCapableReason = "Can't detect valid external address. Please consider using the externalip configuration option if problem persists. Make sure to use IPv4 address only.";
        LogPrintf("CActiveEnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (service.GetPort() != mainnetDefaultPort) {
            nState = ACTIVE_ENODE_NOT_CAPABLE;
            strNotCapableReason = strprintf("Invalid port: %u - only %d is supported on mainnet.", service.GetPort(),
                                            mainnetDefaultPort);
            LogPrintf("CActiveEnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
    } else if (service.GetPort() == mainnetDefaultPort) {
        nState = ACTIVE_ENODE_NOT_CAPABLE;
        strNotCapableReason = strprintf("Invalid port: %u - %d is only supported on mainnet.", service.GetPort(),
                                        mainnetDefaultPort);
        LogPrintf("CActiveEnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    LogPrintf("CActiveEnode::ManageStateInitial -- Checking inbound connection to '%s'\n", service.ToString());
    //TODO
    if (!ConnectNode(CAddress(service, NODE_NETWORK), NULL, false, true)) {
        nState = ACTIVE_ENODE_NOT_CAPABLE;
        strNotCapableReason = "Could not connect to " + service.ToString();
        LogPrintf("CActiveEnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    // Default to REMOTE
    eType = ENODE_REMOTE;

    // Check if wallet funds are available
    if (!pwalletMain) {
        LogPrintf("CActiveEnode::ManageStateInitial -- %s: Wallet not available\n", GetStateString());
        return;
    }

    if (pwalletMain->IsLocked()) {
        LogPrintf("CActiveEnode::ManageStateInitial -- %s: Wallet is locked\n", GetStateString());
        return;
    }

    if (pwalletMain->GetBalance() < ENODE_COIN_REQUIRED * COIN) {
        LogPrintf("CActiveEnode::ManageStateInitial -- %s: Wallet balance is < 1000 P2P\n", GetStateString());
        return;
    }

    // Choose coins to use
    CPubKey pubKeyCollateral;
    CKey keyCollateral;

    // If collateral is found switch to LOCAL mode
    if (pwalletMain->GetEnodeVinAndKeys(vin, pubKeyCollateral, keyCollateral)) {
        eType = ENODE_LOCAL;
    }

    LogPrint("znode", "CActiveEnode::ManageStateInitial -- End status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);
}

void CActiveEnode::ManageStateRemote() {
    LogPrint("znode",
             "CActiveEnode::ManageStateRemote -- Start status = %s, type = %s, pinger enabled = %d, pubKeyEnode.GetID() = %s\n",
             GetStatus(), fPingerEnabled, GetTypeString(), pubKeyEnode.GetID().ToString());

    mnodeman.CheckEnode(pubKeyEnode);
    znode_info_t infoMn = mnodeman.GetEnodeInfo(pubKeyEnode);
    if (infoMn.fInfoValid) {
        if (infoMn.nProtocolVersion != PROTOCOL_VERSION) {
            nState = ACTIVE_ENODE_NOT_CAPABLE;
            strNotCapableReason = "Invalid protocol version";
            LogPrintf("CActiveEnode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if (service != infoMn.addr) {
            nState = ACTIVE_ENODE_NOT_CAPABLE;
            // LogPrintf("service: %s\n", service.ToString());
            // LogPrintf("infoMn.addr: %s\n", infoMn.addr.ToString());
            strNotCapableReason = "Broadcasted IP doesn't match our external address. Make sure you issued a new broadcast if IP of this znode changed recently.";
            LogPrintf("CActiveEnode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if (!CEnode::IsValidStateForAutoStart(infoMn.nActiveState)) {
            nState = ACTIVE_ENODE_NOT_CAPABLE;
            strNotCapableReason = strprintf("Enode in %s state", CEnode::StateToString(infoMn.nActiveState));
            LogPrintf("CActiveEnode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if (nState != ACTIVE_ENODE_STARTED) {
            LogPrintf("CActiveEnode::ManageStateRemote -- STARTED!\n");
            vin = infoMn.vin;
            service = infoMn.addr;
            fPingerEnabled = true;
            nState = ACTIVE_ENODE_STARTED;
        }
    } else {
        nState = ACTIVE_ENODE_NOT_CAPABLE;
        strNotCapableReason = "Enode not in znode list";
        LogPrintf("CActiveEnode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
    }
}

void CActiveEnode::ManageStateLocal() {
    LogPrint("znode", "CActiveEnode::ManageStateLocal -- status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);
    if (nState == ACTIVE_ENODE_STARTED) {
        return;
    }

    // Choose coins to use
    CPubKey pubKeyCollateral;
    CKey keyCollateral;

    if (pwalletMain->GetEnodeVinAndKeys(vin, pubKeyCollateral, keyCollateral)) {
        int nInputAge = GetInputAge(vin);
        if (nInputAge < Params().GetConsensus().nEnodeMinimumConfirmations) {
            nState = ACTIVE_ENODE_INPUT_TOO_NEW;
            strNotCapableReason = strprintf(_("%s - %d confirmations"), GetStatus(), nInputAge);
            LogPrintf("CActiveEnode::ManageStateLocal -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }

        {
            LOCK(pwalletMain->cs_wallet);
            pwalletMain->LockCoin(vin.prevout);
        }

        CEnodeBroadcast mnb;
        std::string strError;
        if (!CEnodeBroadcast::Create(vin, service, keyCollateral, pubKeyCollateral, keyEnode,
                                     pubKeyEnode, strError, mnb)) {
            nState = ACTIVE_ENODE_NOT_CAPABLE;
            strNotCapableReason = "Error creating mastznode broadcast: " + strError;
            LogPrintf("CActiveEnode::ManageStateLocal -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }

        fPingerEnabled = true;
        nState = ACTIVE_ENODE_STARTED;

        //update to znode list
        LogPrintf("CActiveEnode::ManageStateLocal -- Update Enode List\n");
        mnodeman.UpdateEnodeList(mnb);
        mnodeman.NotifyEnodeUpdates();

        //send to all peers
        LogPrintf("CActiveEnode::ManageStateLocal -- Relay broadcast, vin=%s\n", vin.ToString());
        mnb.RelayZNode();
    }
}
