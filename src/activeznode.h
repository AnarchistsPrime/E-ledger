// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ACTIVEENODE_H
#define ACTIVEENODE_H

#include "net.h"
#include "key.h"
#include "wallet/wallet.h"

class CActiveEnode;

static const int ACTIVE_ENODE_INITIAL          = 0; // initial state
static const int ACTIVE_ENODE_SYNC_IN_PROCESS  = 1;
static const int ACTIVE_ENODE_INPUT_TOO_NEW    = 2;
static const int ACTIVE_ENODE_NOT_CAPABLE      = 3;
static const int ACTIVE_ENODE_STARTED          = 4;

extern CActiveEnode activeEnode;

// Responsible for activating the Enode and pinging the network
class CActiveEnode
{
public:
    enum znode_type_enum_t {
        ENODE_UNKNOWN = 0,
        ENODE_REMOTE  = 1,
        ENODE_LOCAL   = 2
    };

private:
    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

    znode_type_enum_t eType;

    bool fPingerEnabled;

    /// Ping Enode
    bool SendEnodePing();

public:
    // Keys for the active Enode
    CPubKey pubKeyEnode;
    CKey keyEnode;

    // Initialized while registering Enode
    CTxIn vin;
    CService service;

    int nState; // should be one of ACTIVE_ENODE_XXXX
    std::string strNotCapableReason;

    CActiveEnode()
        : eType(ENODE_UNKNOWN),
          fPingerEnabled(false),
          pubKeyEnode(),
          keyEnode(),
          vin(),
          service(),
          nState(ACTIVE_ENODE_INITIAL)
    {}

    /// Manage state of active Enode
    void ManageState();

    std::string GetStateString() const;
    std::string GetStatus() const;
    std::string GetTypeString() const;

private:
    void ManageStateInitial();
    void ManageStateRemote();
    void ManageStateLocal();
};

#endif
