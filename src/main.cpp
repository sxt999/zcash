// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Copyright (c) 2015-2020 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "main.h"

#include "addrman.h"
#include "alert.h"
#include "arith_uint256.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "checkqueue.h"
#include "consensus/consensus.h"
#include "consensus/funding.h"
#include "consensus/upgrades.h"
#include "consensus/validation.h"
#include "deprecation.h"
#include "experimental_features.h"
#include "init.h"
#include "key_io.h"
#include "merkleblock.h"
#include "metrics.h"
#include "net.h"
#include "policy/policy.h"
#include "pow.h"
#include "reverse_iterator.h"
#include "txmempool.h"
#include "ui_interface.h"
#include "undo.h"
#include "util.h"
#include "utilmoneystr.h"
#include "validationinterface.h"
#include "wallet/asyncrpcoperation_sendmany.h"
#include "wallet/asyncrpcoperation_shieldcoinbase.h"
#include "warnings.h"

#include <algorithm>
#include <atomic>
#include <sstream>
#include <variant>

#include <boost/algorithm/string/replace.hpp>
#include <boost/math/distributions/poisson.hpp>
#include <boost/thread.hpp>

#include <rust/ed25519.h>
#include <rust/metrics.h>

using namespace std;

#if defined(NDEBUG)
# error "Zcash cannot be compiled without assertions."
#endif

#include "librustzcash.h"

/**
 * Global state
 */

CCriticalSection cs_main;

BlockMap mapBlockIndex;
CChain chainActive;
CBlockIndex *pindexBestHeader = NULL;
static std::atomic<int64_t> nTimeBestReceived(0); // Used only to inform the wallet of when we last received a block
CWaitableCriticalSection csBestBlock;
CConditionVariable cvBlockChange;
int nScriptCheckThreads = 0;
std::atomic_bool fImporting(false);
std::atomic_bool fReindex(false);
bool fTxIndex = false;
bool fAddressIndex = false;     // insightexplorer || lightwalletd
bool fSpentIndex = false;       // insightexplorer
bool fTimestampIndex = false;   // insightexplorer
bool fHavePruned = false;
bool fPruneMode = false;
bool fIsBareMultisigStd = DEFAULT_PERMIT_BAREMULTISIG;
bool fCheckBlockIndex = false;
bool fCheckpointsEnabled = DEFAULT_CHECKPOINTS_ENABLED;
bool fIBDSkipTxVerification = DEFAULT_IBD_SKIP_TX_VERIFICATION;
bool fCoinbaseEnforcedShieldingEnabled = true;
size_t nCoinCacheUsage = 5000 * 300;
uint64_t nPruneTarget = 0;
bool fAlerts = DEFAULT_ALERTS;
int64_t nMaxTipAge = DEFAULT_MAX_TIP_AGE;

std::optional<unsigned int> expiryDeltaArg = std::nullopt;

CFeeRate minRelayTxFee = CFeeRate(DEFAULT_MIN_RELAY_TX_FEE);
CAmount maxTxFee = DEFAULT_TRANSACTION_MAXFEE;

CTxMemPool mempool(::minRelayTxFee);

struct COrphanTx {
    CTransaction tx;
    NodeId fromPeer;
};
map<uint256, COrphanTx> mapOrphanTransactions GUARDED_BY(cs_main);;
map<uint256, set<uint256> > mapOrphanTransactionsByPrev GUARDED_BY(cs_main);;
void EraseOrphansFor(NodeId peer) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

/**
 * Returns true if there are nRequired or more blocks of minVersion or above
 * in the last Consensus::Params::nMajorityWindow blocks, starting at pstart and going backwards.
 */
static bool IsSuperMajority(int minVersion, const CBlockIndex* pstart, unsigned nRequired, const Consensus::Params& consensusParams);
static void CheckBlockIndex(const Consensus::Params& consensusParams);

/** Constant stuff for coinbase transactions we create: */
CScript COINBASE_FLAGS;

const string strMessageMagic = "Zcash Signed Message:\n";

// Internal stuff
namespace {

    unsigned char ShieldedReqRejectCode(UnsatisfiedShieldedReq shieldedReq)
    {
        switch (shieldedReq) {
            case UnsatisfiedShieldedReq::SproutDuplicateNullifier:
            case UnsatisfiedShieldedReq::SaplingDuplicateNullifier:
            case UnsatisfiedShieldedReq::OrchardDuplicateNullifier:
                return REJECT_DUPLICATE;
            case UnsatisfiedShieldedReq::SproutUnknownAnchor:
            case UnsatisfiedShieldedReq::SaplingUnknownAnchor:
            case UnsatisfiedShieldedReq::OrchardUnknownAnchor:
                return REJECT_INVALID;
        }
    }

    std::string ShieldedReqRejectReason(UnsatisfiedShieldedReq shieldedReq)
    {
        switch (shieldedReq) {
            case UnsatisfiedShieldedReq::SproutDuplicateNullifier:  return "bad-txns-sprout-duplicate-nullifier";
            case UnsatisfiedShieldedReq::SproutUnknownAnchor:       return "bad-txns-sprout-unknown-anchor";
            case UnsatisfiedShieldedReq::SaplingDuplicateNullifier: return "bad-txns-sapling-duplicate-nullifier";
            case UnsatisfiedShieldedReq::SaplingUnknownAnchor:      return "bad-txns-sapling-unknown-anchor";
            case UnsatisfiedShieldedReq::OrchardDuplicateNullifier: return "bad-txns-orchard-duplicate-nullifier";
            case UnsatisfiedShieldedReq::OrchardUnknownAnchor:      return "bad-txns-orchard-unknown-anchor";
        }
    }

    /** Abort with a message */
    bool AbortNode(const std::string& strMessage, const std::string& userMessage="")
    {
        SetMiscWarning(strMessage, GetTime());
        LogPrintf("*** %s\n", strMessage);
        uiInterface.ThreadSafeMessageBox(
            userMessage.empty() ? _("Error: A fatal internal error occurred, see debug.log for details") : userMessage,
            "", CClientUIInterface::MSG_ERROR);
        StartShutdown();
        return false;
    }

    bool AbortNode(CValidationState& state, const std::string& strMessage, const std::string& userMessage="")
    {
        AbortNode(strMessage, userMessage);
        return state.Error(strMessage);
    }

    struct CBlockIndexWorkComparator
    {
        bool operator()(CBlockIndex *pa, CBlockIndex *pb) const {
            // First sort by most total work, ...
            if (pa->nChainWork > pb->nChainWork) return false;
            if (pa->nChainWork < pb->nChainWork) return true;

            // ... then by earliest time received, ...
            if (pa->nSequenceId < pb->nSequenceId) return false;
            if (pa->nSequenceId > pb->nSequenceId) return true;

            // Use pointer address as tie breaker (should only happen with blocks
            // loaded from disk, as those all have id 0).
            if (pa < pb) return false;
            if (pa > pb) return true;

            // Identical blocks.
            return false;
        }
    };

    CBlockIndex *pindexBestInvalid;

    /**
     * The set of all CBlockIndex entries with BLOCK_VALID_TRANSACTIONS (for itself and all ancestors) and
     * as good as our current tip or better. Entries may be failed, though, and pruning nodes may be
     * missing the data for the block.
     */
    set<CBlockIndex*, CBlockIndexWorkComparator> setBlockIndexCandidates;
    /** Number of nodes with fSyncStarted. */
    int nSyncStarted = 0;
    /** All pairs A->B, where A (or one if its ancestors) misses transactions, but B has transactions.
      * Pruned nodes may have entries where B is missing data.
      */
    multimap<CBlockIndex*, CBlockIndex*> mapBlocksUnlinked;

    CCriticalSection cs_LastBlockFile;
    std::vector<CBlockFileInfo> vinfoBlockFile;
    int nLastBlockFile = 0;
    /** Global flag to indicate we should check to see if there are
     *  block/undo files that should be deleted.  Set on startup
     *  or if we allocate more file space when we're in prune mode
     */
    bool fCheckForPruning = false;

    /**
     * Every received block is assigned a unique and increasing identifier, so we
     * know which one to give priority in case of a fork.
     */
    CCriticalSection cs_nBlockSequenceId;
    /** Blocks loaded from disk are assigned id 0, so start the counter at 1. */
    uint32_t nBlockSequenceId = 1;

    /**
     * Sources of received blocks, saved to be able to send them reject
     * messages or ban them when processing happens afterwards. Protected by
     * cs_main.
     */
    map<uint256, NodeId> mapBlockSource;

    /**
     * Filter for transactions that were recently rejected by
     * AcceptToMemoryPool. These are not rerequested until the chain tip
     * changes, at which point the entire filter is reset. Protected by
     * cs_main.
     *
     * Without this filter we'd be re-requesting txs from each of our peers,
     * increasing bandwidth consumption considerably. For instance, with 100
     * peers, half of which relay a tx we don't accept, that might be a 50x
     * bandwidth increase. A flooding attacker attempting to roll-over the
     * filter using minimum-sized, 60byte, transactions might manage to send
     * 1000/sec if we have fast peers, so we pick 120,000 to give our peers a
     * two minute window to send invs to us.
     *
     * Decreasing the false positive rate is fairly cheap, so we pick one in a
     * million to make it highly unlikely for users to have issues with this
     * filter.
     *
     * Memory used: 1.3 MB
     */
    boost::scoped_ptr<CRollingBloomFilter> recentRejects;
    uint256 hashRecentRejectsChainTip;

    /** Blocks that are in flight, and that are in the queue to be downloaded. Protected by cs_main. */
    struct QueuedBlock {
        uint256 hash;
        CBlockIndex* pindex;     //!< Optional.
        int64_t nTime;           //!< Time of "getdata" request in microseconds.
        bool fValidatedHeaders;  //!< Whether this block has validated headers at the time of request.
        int64_t nTimeDisconnect; //!< The timeout for this block request (for disconnecting a slow peer)
    };
    map<uint256, pair<NodeId, list<QueuedBlock>::iterator> > mapBlocksInFlight;

    /** Number of blocks in flight with validated headers. */
    int nQueuedValidatedHeaders = 0;

    /** Number of preferable block download peers. */
    int nPreferredDownload = 0;

    /** Dirty block index entries. */
    set<CBlockIndex*> setDirtyBlockIndex;

    /** Dirty block file entries. */
    set<int> setDirtyFileInfo;
} // anon namespace

//////////////////////////////////////////////////////////////////////////////
//
// Registration of network node signals.
//

namespace {

struct CBlockReject {
    unsigned char chRejectCode;
    string strRejectReason;
    uint256 hashBlock;
};

/**
 * Maintain validation-specific state about nodes, protected by cs_main, instead
 * by CNode's own locks. This simplifies asynchronous operation, where
 * processing of incoming data is done after the ProcessMessage call returns,
 * and we're no longer holding the node's locks.
 */
struct CNodeState {
    //! The peer's address
    CService address;
    //! Whether we have a fully established connection.
    bool fCurrentlyConnected;
    //! Accumulated misbehaviour score for this peer.
    int nMisbehavior;
    //! Whether this peer should be disconnected and banned (unless whitelisted).
    bool fShouldBan;
    //! String name of this peer (debugging/logging purposes).
    std::string name;
    //! List of asynchronously-determined block rejections to notify this peer about.
    std::vector<CBlockReject> rejects;
    //! The best known block we know this peer has announced.
    CBlockIndex *pindexBestKnownBlock;
    //! The hash of the last unknown block this peer has announced.
    uint256 hashLastUnknownBlock;
    //! The last full block we both have.
    CBlockIndex *pindexLastCommonBlock;
    //! Whether we've started headers synchronization with this peer.
    bool fSyncStarted;
    //! Since when we're stalling block download progress (in microseconds), or 0.
    int64_t nStallingSince;
    list<QueuedBlock> vBlocksInFlight;
    int nBlocksInFlight;
    int nBlocksInFlightValidHeaders;
    //! Whether we consider this a preferred download peer.
    bool fPreferredDownload;

    CNodeState() {
        fCurrentlyConnected = false;
        nMisbehavior = 0;
        fShouldBan = false;
        pindexBestKnownBlock = NULL;
        hashLastUnknownBlock.SetNull();
        pindexLastCommonBlock = NULL;
        fSyncStarted = false;
        nStallingSince = 0;
        nBlocksInFlight = 0;
        nBlocksInFlightValidHeaders = 0;
        fPreferredDownload = false;
    }
};

/** Map maintaining per-node state. Requires cs_main. */
map<NodeId, CNodeState> mapNodeState;

// Requires cs_main.
CNodeState *State(NodeId pnode) {
    map<NodeId, CNodeState>::iterator it = mapNodeState.find(pnode);
    if (it == mapNodeState.end())
        return NULL;
    return &it->second;
}

int GetHeight()
{
    LOCK(cs_main);
    return chainActive.Height();
}

void UpdatePreferredDownload(CNode* node, CNodeState* state)
{
    nPreferredDownload -= state->fPreferredDownload;

    // Whether this node should be marked as a preferred download node.
    state->fPreferredDownload = (!node->fInbound || node->fWhitelisted) && !node->fOneShot && !node->fClient;

    nPreferredDownload += state->fPreferredDownload;
}

// Returns time at which to timeout block request (nTime in microseconds)
int64_t GetBlockTimeout(int64_t nTime, int nValidatedQueuedBefore, const Consensus::Params &consensusParams, int nHeight)
{
    return nTime + 500000 * consensusParams.PoWTargetSpacing(nHeight) * (4 + nValidatedQueuedBefore);
}

void InitializeNode(NodeId nodeid, const CNode *pnode) {
    LOCK(cs_main);
    CNodeState &state = mapNodeState.insert(std::make_pair(nodeid, CNodeState())).first->second;
    state.name = pnode->GetAddrName();
    state.address = pnode->addr;
}

void FinalizeNode(NodeId nodeid) {
    LOCK(cs_main);
    CNodeState *state = State(nodeid);

    if (state->fSyncStarted)
        nSyncStarted--;

    if (state->nMisbehavior == 0 && state->fCurrentlyConnected) {
        AddressCurrentlyConnected(state->address);
    }

    for (const QueuedBlock& entry : state->vBlocksInFlight)
        mapBlocksInFlight.erase(entry.hash);
    EraseOrphansFor(nodeid);
    nPreferredDownload -= state->fPreferredDownload;

    mapNodeState.erase(nodeid);
}

// Requires cs_main.
// Returns a bool indicating whether we requested this block.
bool MarkBlockAsReceived(const uint256& hash) {
    map<uint256, pair<NodeId, list<QueuedBlock>::iterator> >::iterator itInFlight = mapBlocksInFlight.find(hash);
    if (itInFlight != mapBlocksInFlight.end()) {
        CNodeState *state = State(itInFlight->second.first);
        nQueuedValidatedHeaders -= itInFlight->second.second->fValidatedHeaders;
        state->nBlocksInFlightValidHeaders -= itInFlight->second.second->fValidatedHeaders;
        state->vBlocksInFlight.erase(itInFlight->second.second);
        state->nBlocksInFlight--;
        state->nStallingSince = 0;
        mapBlocksInFlight.erase(itInFlight);
        return true;
    }
    return false;
}

// Requires cs_main.
void MarkBlockAsInFlight(NodeId nodeid, const uint256& hash, const Consensus::Params& consensusParams, CBlockIndex *pindex = NULL) {
    CNodeState *state = State(nodeid);
    assert(state != NULL);

    // Make sure it's not listed somewhere already.
    MarkBlockAsReceived(hash);

    int64_t nNow = GetTimeMicros();
    int nHeight = pindex != NULL ? pindex->nHeight : chainActive.Height(); // Help block timeout computation
    QueuedBlock newentry = {hash, pindex, nNow, pindex != NULL, GetBlockTimeout(nNow, nQueuedValidatedHeaders, consensusParams, nHeight)};
    nQueuedValidatedHeaders += newentry.fValidatedHeaders;
    list<QueuedBlock>::iterator it = state->vBlocksInFlight.insert(state->vBlocksInFlight.end(), newentry);
    state->nBlocksInFlight++;
    state->nBlocksInFlightValidHeaders += newentry.fValidatedHeaders;
    mapBlocksInFlight[hash] = std::make_pair(nodeid, it);
}

/** Check whether the last unknown block a peer advertized is not yet known. */
void ProcessBlockAvailability(NodeId nodeid) {
    CNodeState *state = State(nodeid);
    assert(state != NULL);

    if (!state->hashLastUnknownBlock.IsNull()) {
        BlockMap::iterator itOld = mapBlockIndex.find(state->hashLastUnknownBlock);
        if (itOld != mapBlockIndex.end() && itOld->second->nChainWork > 0) {
            if (state->pindexBestKnownBlock == NULL || itOld->second->nChainWork >= state->pindexBestKnownBlock->nChainWork)
                state->pindexBestKnownBlock = itOld->second;
            state->hashLastUnknownBlock.SetNull();
        }
    }
}

/** Update tracking information about which blocks a peer is assumed to have. */
void UpdateBlockAvailability(NodeId nodeid, const uint256 &hash) {
    CNodeState *state = State(nodeid);
    assert(state != NULL);

    ProcessBlockAvailability(nodeid);

    BlockMap::iterator it = mapBlockIndex.find(hash);
    if (it != mapBlockIndex.end() && it->second->nChainWork > 0) {
        // An actually better block was announced.
        if (state->pindexBestKnownBlock == NULL || it->second->nChainWork >= state->pindexBestKnownBlock->nChainWork)
            state->pindexBestKnownBlock = it->second;
    } else {
        // An unknown block was announced; just assume that the latest one is the best one.
        state->hashLastUnknownBlock = hash;
    }
}

/** Find the last common ancestor two blocks have.
 *  Both pa and pb must be non-NULL. */
CBlockIndex* LastCommonAncestor(CBlockIndex* pa, CBlockIndex* pb) {
    if (pa->nHeight > pb->nHeight) {
        pa = pa->GetAncestor(pb->nHeight);
    } else if (pb->nHeight > pa->nHeight) {
        pb = pb->GetAncestor(pa->nHeight);
    }

    while (pa != pb && pa && pb) {
        pa = pa->pprev;
        pb = pb->pprev;
    }

    // Eventually all chain branches meet at the genesis block.
    assert(pa == pb);
    return pa;
}

/** Update pindexLastCommonBlock and add not-in-flight missing successors to vBlocks, until it has
 *  at most count entries. */
void FindNextBlocksToDownload(NodeId nodeid, unsigned int count, std::vector<CBlockIndex*>& vBlocks, NodeId& nodeStaller) {
    if (count == 0)
        return;

    vBlocks.reserve(vBlocks.size() + count);
    CNodeState *state = State(nodeid);
    assert(state != NULL);

    // Make sure pindexBestKnownBlock is up to date, we'll need it.
    ProcessBlockAvailability(nodeid);

    if (state->pindexBestKnownBlock == NULL || state->pindexBestKnownBlock->nChainWork < chainActive.Tip()->nChainWork) {
        // This peer has nothing interesting.
        return;
    }

    if (state->pindexLastCommonBlock == NULL) {
        // Bootstrap quickly by guessing a parent of our best tip is the forking point.
        // Guessing wrong in either direction is not a problem.
        state->pindexLastCommonBlock = chainActive[std::min(state->pindexBestKnownBlock->nHeight, chainActive.Height())];
    }

    // If the peer reorganized, our previous pindexLastCommonBlock may not be an ancestor
    // of its current tip anymore. Go back enough to fix that.
    state->pindexLastCommonBlock = LastCommonAncestor(state->pindexLastCommonBlock, state->pindexBestKnownBlock);
    if (state->pindexLastCommonBlock == state->pindexBestKnownBlock)
        return;

    std::vector<CBlockIndex*> vToFetch;
    CBlockIndex *pindexWalk = state->pindexLastCommonBlock;
    // Never fetch further than the best block we know the peer has, or more than BLOCK_DOWNLOAD_WINDOW + 1 beyond the last
    // linked block we have in common with this peer. The +1 is so we can detect stalling, namely if we would be able to
    // download that next block if the window were 1 larger.
    int nWindowEnd = state->pindexLastCommonBlock->nHeight + BLOCK_DOWNLOAD_WINDOW;
    int nMaxHeight = std::min<int>(state->pindexBestKnownBlock->nHeight, nWindowEnd + 1);
    NodeId waitingfor = -1;
    while (pindexWalk->nHeight < nMaxHeight) {
        // Read up to 128 (or more, if more blocks than that are needed) successors of pindexWalk (towards
        // pindexBestKnownBlock) into vToFetch. We fetch 128, because CBlockIndex::GetAncestor may be as expensive
        // as iterating over ~100 CBlockIndex* entries anyway.
        int nToFetch = std::min(nMaxHeight - pindexWalk->nHeight, std::max<int>(count - vBlocks.size(), 128));
        vToFetch.resize(nToFetch);
        pindexWalk = state->pindexBestKnownBlock->GetAncestor(pindexWalk->nHeight + nToFetch);
        vToFetch[nToFetch - 1] = pindexWalk;
        for (unsigned int i = nToFetch - 1; i > 0; i--) {
            vToFetch[i - 1] = vToFetch[i]->pprev;
        }

        // Iterate over those blocks in vToFetch (in forward direction), adding the ones that
        // are not yet downloaded and not in flight to vBlocks. In the meantime, update
        // pindexLastCommonBlock as long as all ancestors are already downloaded, or if it's
        // already part of our chain (and therefore don't need it even if pruned).
        for (CBlockIndex* pindex : vToFetch) {
            if (!pindex->IsValid(BLOCK_VALID_TREE)) {
                // We consider the chain that this peer is on invalid.
                return;
            }
            if (pindex->nStatus & BLOCK_HAVE_DATA || chainActive.Contains(pindex)) {
                if (pindex->nChainTx)
                    state->pindexLastCommonBlock = pindex;
            } else if (mapBlocksInFlight.count(pindex->GetBlockHash()) == 0) {
                // The block is not already downloaded, and not yet in flight.
                if (pindex->nHeight > nWindowEnd) {
                    // We reached the end of the window.
                    if (vBlocks.size() == 0 && waitingfor != nodeid) {
                        // We aren't able to fetch anything, but we would be if the download window was one larger.
                        nodeStaller = waitingfor;
                    }
                    return;
                }
                vBlocks.push_back(pindex);
                if (vBlocks.size() == count) {
                    return;
                }
            } else if (waitingfor == -1) {
                // This is the first already-in-flight block.
                waitingfor = mapBlocksInFlight[pindex->GetBlockHash()].first;
            }
        }
    }
}

} // anon namespace

bool GetNodeStateStats(NodeId nodeid, CNodeStateStats &stats) {
    LOCK(cs_main);
    CNodeState *state = State(nodeid);
    if (state == NULL)
        return false;
    stats.nMisbehavior = state->nMisbehavior;
    stats.nSyncHeight = state->pindexBestKnownBlock ? state->pindexBestKnownBlock->nHeight : -1;
    stats.nCommonHeight = state->pindexLastCommonBlock ? state->pindexLastCommonBlock->nHeight : -1;
    for (const QueuedBlock& queue : state->vBlocksInFlight) {
        if (queue.pindex)
            stats.vHeightInFlight.push_back(queue.pindex->nHeight);
    }
    return true;
}

void RegisterNodeSignals(CNodeSignals& nodeSignals)
{
    nodeSignals.GetHeight.connect(&GetHeight);
    nodeSignals.ProcessMessages.connect(&ProcessMessages);
    nodeSignals.SendMessages.connect(&SendMessages);
    nodeSignals.InitializeNode.connect(&InitializeNode);
    nodeSignals.FinalizeNode.connect(&FinalizeNode);
}

void UnregisterNodeSignals(CNodeSignals& nodeSignals)
{
    nodeSignals.GetHeight.disconnect(&GetHeight);
    nodeSignals.ProcessMessages.disconnect(&ProcessMessages);
    nodeSignals.SendMessages.disconnect(&SendMessages);
    nodeSignals.InitializeNode.disconnect(&InitializeNode);
    nodeSignals.FinalizeNode.disconnect(&FinalizeNode);
}

CBlockIndex* FindForkInGlobalIndex(const CChain& chain, const CBlockLocator& locator)
{
    // Find the first block the caller has in the main chain
    for (const uint256& hash : locator.vHave) {
        BlockMap::iterator mi = mapBlockIndex.find(hash);
        if (mi != mapBlockIndex.end())
        {
            CBlockIndex* pindex = (*mi).second;
            if (chain.Contains(pindex))
                return pindex;
            if (pindex->GetAncestor(chain.Height()) == chain.Tip()) {
                return chain.Tip();
            }
        }
    }
    return chain.Genesis();
}

CCoinsViewCache *pcoinsTip = NULL;
CBlockTreeDB *pblocktree = NULL;

//////////////////////////////////////////////////////////////////////////////
//
// mapOrphanTransactions
//

bool AddOrphanTx(const CTransaction& tx, NodeId peer) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    uint256 hash = tx.GetHash();
    if (mapOrphanTransactions.count(hash))
        return false;

    // Ignore big transactions, to avoid a
    // send-big-orphans memory exhaustion attack. If a peer has a legitimate
    // large transaction with a missing parent then we assume
    // it will rebroadcast it later, after the parent transaction(s)
    // have been mined or received.
    // 10,000 orphans, each of which is at most 5,000 bytes big is
    // at most 500 megabytes of orphans:
    unsigned int sz = GetSerializeSize(tx, SER_NETWORK, tx.nVersion);
    if (sz > 5000)
    {
        LogPrint("mempool", "ignoring large orphan tx (size: %u, hash: %s)\n", sz, hash.ToString());
        return false;
    }

    mapOrphanTransactions[hash].tx = tx;
    mapOrphanTransactions[hash].fromPeer = peer;
    for (const CTxIn& txin : tx.vin)
        mapOrphanTransactionsByPrev[txin.prevout.hash].insert(hash);

    LogPrint("mempool", "stored orphan tx %s (mapsz %u prevsz %u)\n", hash.ToString(),
             mapOrphanTransactions.size(), mapOrphanTransactionsByPrev.size());
    return true;
}

void static EraseOrphanTx(uint256 hash) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    map<uint256, COrphanTx>::iterator it = mapOrphanTransactions.find(hash);
    if (it == mapOrphanTransactions.end())
        return;
    for (const CTxIn& txin : it->second.tx.vin)
    {
        map<uint256, set<uint256> >::iterator itPrev = mapOrphanTransactionsByPrev.find(txin.prevout.hash);
        if (itPrev == mapOrphanTransactionsByPrev.end())
            continue;
        itPrev->second.erase(hash);
        if (itPrev->second.empty())
            mapOrphanTransactionsByPrev.erase(itPrev);
    }
    mapOrphanTransactions.erase(it);
}

void EraseOrphansFor(NodeId peer)
{
    int nErased = 0;
    map<uint256, COrphanTx>::iterator iter = mapOrphanTransactions.begin();
    while (iter != mapOrphanTransactions.end())
    {
        map<uint256, COrphanTx>::iterator maybeErase = iter++; // increment to avoid iterator becoming invalid
        if (maybeErase->second.fromPeer == peer)
        {
            EraseOrphanTx(maybeErase->second.tx.GetHash());
            ++nErased;
        }
    }
    if (nErased > 0) LogPrint("mempool", "Erased %d orphan tx from peer %d\n", nErased, peer);
}


unsigned int LimitOrphanTxSize(unsigned int nMaxOrphans) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    unsigned int nEvicted = 0;
    while (mapOrphanTransactions.size() > nMaxOrphans)
    {
        // Evict a random orphan:
        uint256 randomhash = GetRandHash();
        map<uint256, COrphanTx>::iterator it = mapOrphanTransactions.lower_bound(randomhash);
        if (it == mapOrphanTransactions.end())
            it = mapOrphanTransactions.begin();
        EraseOrphanTx(it->first);
        ++nEvicted;
    }
    return nEvicted;
}

bool IsFinalTx(const CTransaction &tx, int nBlockHeight, int64_t nBlockTime)
{
    if (tx.nLockTime == 0)
        return true;
    if ((int64_t)tx.nLockTime < ((int64_t)tx.nLockTime < LOCKTIME_THRESHOLD ? (int64_t)nBlockHeight : nBlockTime))
        return true;
    for (const CTxIn& txin : tx.vin)
        if (!txin.IsFinal())
            return false;
    return true;
}

bool IsExpiredTx(const CTransaction &tx, int nBlockHeight)
{
    if (tx.nExpiryHeight == 0 || tx.IsCoinBase()) {
        return false;
    }
    return static_cast<uint32_t>(nBlockHeight) > tx.nExpiryHeight;
}

bool IsExpiringSoonTx(const CTransaction &tx, int nNextBlockHeight)
{
    return IsExpiredTx(tx, nNextBlockHeight + TX_EXPIRING_SOON_THRESHOLD);
}

bool CheckFinalTx(const CTransaction &tx, int flags)
{
    AssertLockHeld(cs_main);

    // By convention a negative value for flags indicates that the
    // current network-enforced consensus rules should be used. In
    // a future soft-fork scenario that would mean checking which
    // rules would be enforced for the next block and setting the
    // appropriate flags. At the present time no soft-forks are
    // scheduled, so no flags are set.
    flags = std::max(flags, 0);

    // CheckFinalTx() uses chainActive.Height()+1 to evaluate
    // nLockTime because when IsFinalTx() is called within
    // CBlock::AcceptBlock(), the height of the block *being*
    // evaluated is what is used. Thus if we want to know if a
    // transaction can be part of the *next* block, we need to call
    // IsFinalTx() with one more than chainActive.Height().
    const int nBlockHeight = chainActive.Height() + 1;

    // Timestamps on the other hand don't get any special treatment,
    // because we can't know what timestamp the next block will have,
    // and there aren't timestamp applications where it matters.
    // However this changes once median past time-locks are enforced:
    const int64_t nBlockTime = (flags & LOCKTIME_MEDIAN_TIME_PAST)
                             ? chainActive.Tip()->GetMedianTimePast()
                             : GetTime();

    return IsFinalTx(tx, nBlockHeight, nBlockTime);
}

unsigned int GetLegacySigOpCount(const CTransaction& tx)
{
    unsigned int nSigOps = 0;
    for (const CTxIn& txin : tx.vin)
    {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    }
    for (const CTxOut& txout : tx.vout)
    {
        nSigOps += txout.scriptPubKey.GetSigOpCount(false);
    }
    return nSigOps;
}

unsigned int GetP2SHSigOpCount(const CTransaction& tx, const CCoinsViewCache& inputs)
{
    if (tx.IsCoinBase())
        return 0;

    unsigned int nSigOps = 0;
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        const CTxOut &prevout = inputs.GetOutputFor(tx.vin[i]);
        if (prevout.scriptPubKey.IsPayToScriptHash())
            nSigOps += prevout.scriptPubKey.GetSigOpCount(tx.vin[i].scriptSig);
    }
    return nSigOps;
}

/**
 * Check a transaction contextually against a set of consensus rules valid at a given block height.
 *
 * Notes:
 * 1. AcceptToMemoryPool calls CheckTransaction and this function.
 * 2. ProcessNewBlock calls AcceptBlock, which calls CheckBlock (which calls CheckTransaction)
 *    and ContextualCheckBlock (which calls this function).
 * 3. For consensus rules that relax restrictions (where a transaction that is invalid at
 *    nHeight can become valid at a later height), we make the bans conditional on not
 *    being in Initial Block Download mode.
 * 4. The isInitBlockDownload argument is a function parameter to assist with testing.
 */
bool ContextualCheckTransaction(
        const CTransaction& tx,
        CValidationState &state,
        const CChainParams& chainparams,
        const int nHeight,
        const bool isMined,
        bool (*isInitBlockDownload)(const Consensus::Params&))
{
    const int DOS_LEVEL_BLOCK = 100;
    // DoS level set to 10 to be more forgiving.
    const int DOS_LEVEL_MEMPOOL = 10;

    // For constricting rules, we don't need to account for IBD mode.
    auto dosLevelConstricting = isMined ? DOS_LEVEL_BLOCK : DOS_LEVEL_MEMPOOL;
    // For rules that are relaxing (or might become relaxing when a future
    // network upgrade is implemented), we need to account for IBD mode.
    auto dosLevelPotentiallyRelaxing = isMined ? DOS_LEVEL_BLOCK : (
        isInitBlockDownload(chainparams.GetConsensus()) ? 0 : DOS_LEVEL_MEMPOOL);

    auto consensus = chainparams.GetConsensus();
    auto consensusBranchId = CurrentEpochBranchId(nHeight, consensus);

    bool overwinterActive = consensus.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_OVERWINTER);
    bool saplingActive = consensus.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_SAPLING);
    bool beforeOverwinter = !overwinterActive;
    bool heartwoodActive = consensus.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_HEARTWOOD);
    bool canopyActive = consensus.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_CANOPY);
    bool nu5Active = consensus.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_NU5);
    bool futureActive = consensus.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_ZFUTURE);

    assert(!saplingActive || overwinterActive); // Sapling cannot be active unless Overwinter is
    assert(!heartwoodActive || saplingActive);  // Heartwood cannot be active unless Sapling is
    assert(!canopyActive || heartwoodActive);   // Canopy cannot be active unless Heartwood is
    assert(!nu5Active || canopyActive);         // NU5 cannot be active unless Canopy is
    assert(!futureActive || nu5Active);         // ZFUTURE must include consensus rules for all supported network upgrades.

    auto const orchard_bundle = tx.GetOrchardBundle();

    // Rules that apply only to Sprout
    if (beforeOverwinter) {
        // Reject transactions which are intended for Overwinter and beyond
        if (tx.fOverwintered) {
            return state.DoS(
                dosLevelPotentiallyRelaxing,
                error("ContextualCheckTransaction(): overwinter is not active yet"),
                REJECT_INVALID, "tx-overwinter-not-active");
        }
    }

    // Rules that apply to Overwinter and later:
    if (overwinterActive) {
        // Reject transactions intended for Sprout
        if (!tx.fOverwintered) {
            return state.DoS(
                dosLevelConstricting,
                error("ContextualCheckTransaction(): fOverwintered flag must be set when Overwinter is active"),
                REJECT_INVALID, "tx-overwintered-flag-not-set");
        }

        // The condition `if (tx.nVersion < OVERWINTER_MIN_TX_VERSION)`
        // that would otherwise fire here is already performed in the
        // noncontextual checks in CheckTransactionWithoutProofVerification

        // Check that all transactions are unexpired
        if (IsExpiredTx(tx, nHeight)) {
            // Don't increase banscore if the transaction only just expired
            int expiredDosLevel = IsExpiredTx(tx, nHeight - 1) ? dosLevelConstricting : 0;
            return state.DoS(
                    expiredDosLevel,
                    error("ContextualCheckTransaction(): transaction is expired. Resending when caught up with the blockchain, or manually setting the zcashd txexpirydelta parameter may help."),
                    REJECT_INVALID, "tx-overwinter-expired");
        }

        // Rules that became inactive after Sapling activation.
        if (!saplingActive) {
            // Reject transactions with invalid version
            // OVERWINTER_MIN_TX_VERSION is checked against as a non-contextual check.
            if (tx.nVersion > OVERWINTER_MAX_TX_VERSION) {
                return state.DoS(
                    dosLevelPotentiallyRelaxing,
                    error("ContextualCheckTransaction(): overwinter version too high"),
                    REJECT_INVALID, "bad-tx-overwinter-version-too-high");
            }

            // Reject transactions with non-Overwinter version group ID
            if (tx.nVersionGroupId != OVERWINTER_VERSION_GROUP_ID) {
                return state.DoS(
                    dosLevelPotentiallyRelaxing,
                    error("ContextualCheckTransaction(): invalid Overwinter tx version"),
                    REJECT_INVALID, "bad-overwinter-tx-version-group-id");
            }
        }
    }

    // Rules that apply to Sapling and later:
    if (saplingActive) {
        // Reject transactions with invalid version
        if (tx.nVersionGroupId == SAPLING_VERSION_GROUP_ID) {
            if (tx.nVersion < SAPLING_MIN_TX_VERSION) {
                return state.DoS(
                    dosLevelConstricting,
                    error("ContextualCheckTransaction(): Sapling version too low"),
                    REJECT_INVALID, "bad-tx-sapling-version-too-low");
            }

            if (tx.nVersion > SAPLING_MAX_TX_VERSION) {
                return state.DoS(
                    dosLevelPotentiallyRelaxing,
                    error("ContextualCheckTransaction(): Sapling version too high"),
                    REJECT_INVALID, "bad-tx-sapling-version-too-high");
            }
        }

        // Rules that became inactive after NU5 activation.
        if (!nu5Active) {
            // Reject transactions with invalid version group id
            if (tx.nVersionGroupId != SAPLING_VERSION_GROUP_ID) {
                return state.DoS(
                    dosLevelPotentiallyRelaxing,
                    error("ContextualCheckTransaction(): invalid Sapling tx version"),
                    REJECT_INVALID, "bad-sapling-tx-version-group-id");
            }
        }
    } else {
        // Rules that apply generally before Sapling. These were
        // previously noncontextual checks that became contextual
        // after Sapling activation.

        // Reject transactions that exceed pre-sapling size limits
        static_assert(MAX_BLOCK_SIZE > MAX_TX_SIZE_BEFORE_SAPLING); // sanity
        if (::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION) > MAX_TX_SIZE_BEFORE_SAPLING)
            return state.DoS(
                dosLevelPotentiallyRelaxing,
                error("ContextualCheckTransaction(): size limits failed"),
                REJECT_INVALID, "bad-txns-oversize");
    }

    // From Canopy onward, coinbase transaction must include outputs corresponding to the
    // ZIP 207 consensus funding streams active at the current block height. To avoid
    // double-decrypting, we detect any shielded funding streams during the Heartwood
    // consensus check. If Canopy is not yet active, fundingStreamElements will be empty.
    std::set<Consensus::FundingStreamElement> fundingStreamElements = Consensus::GetActiveFundingStreamElements(
        nHeight,
        GetBlockSubsidy(nHeight, consensus),
        consensus);

    // Rules that apply to Heartwood and later:
    if (heartwoodActive) {
        if (tx.IsCoinBase()) {
            // All Sapling outputs in coinbase transactions MUST have valid note commitments
            // when recovered using a 32-byte array of zeroes as the outgoing viewing key.
            // https://zips.z.cash/zip-0213#specification
            uint256 ovk;
            for (const OutputDescription &output : tx.vShieldedOutput) {
                auto outPlaintext = SaplingOutgoingPlaintext::decrypt(
                    output.outCiphertext, ovk, output.cv, output.cmu, output.ephemeralKey);
                if (!outPlaintext) {
                    return state.DoS(
                        DOS_LEVEL_BLOCK,
                        error("ContextualCheckTransaction(): coinbase output description has invalid outCiphertext"),
                        REJECT_INVALID, "bad-cb-output-desc-invalid-outct");
                }

                // SaplingNotePlaintext::decrypt() checks note commitment validity.
                auto encPlaintext = SaplingNotePlaintext::decrypt(
                    consensus,
                    nHeight,
                    output.encCiphertext,
                    output.ephemeralKey,
                    outPlaintext->esk,
                    outPlaintext->pk_d,
                    output.cmu);

                if (!encPlaintext) {
                    return state.DoS(
                        DOS_LEVEL_BLOCK,
                        error("ContextualCheckTransaction(): coinbase output description has invalid encCiphertext"),
                        REJECT_INVALID, "bad-cb-output-desc-invalid-encct");
                }

                // ZIP 207: detect shielded funding stream elements
                if (canopyActive) {
                    libzcash::SaplingPaymentAddress zaddr(encPlaintext->d, outPlaintext->pk_d);
                    for (auto it = fundingStreamElements.begin(); it != fundingStreamElements.end(); ++it) {
                        const libzcash::SaplingPaymentAddress* streamAddr = std::get_if<libzcash::SaplingPaymentAddress>(&(it->first));
                        if (streamAddr && zaddr == *streamAddr && encPlaintext->value() == it->second) {
                            fundingStreamElements.erase(it);
                            break;
                        }
                    }
                }

                // ZIP 212: after ZIP 212 any Sapling output of a coinbase tx that is
                // decrypted to a note plaintext, MUST have note plaintext lead byte equal
                // to 0x02. This applies even during the grace period, and also applies to
                // funding stream outputs sent to shielded payment addresses, if any.
                // https://zips.z.cash/zip-0212#consensus-rule-change-for-coinbase-transactions
                auto leadByte = encPlaintext->get_leadbyte();
                assert(leadByte == 0x01 || leadByte == 0x02);
                if (canopyActive != (leadByte == 0x02)) {
                    return state.DoS(
                        DOS_LEVEL_BLOCK,
                        error("ContextualCheckTransaction(): coinbase output description has invalid note plaintext version"),
                        REJECT_INVALID,
                        "bad-cb-output-desc-invalid-note-plaintext-version");
                }
            }
        }
    } else {
        // Rules that apply generally before Heartwood. These were
        // previously noncontextual checks that became contextual
        // after Heartwood activation.

        if (tx.IsCoinBase()) {
            // A coinbase transaction cannot have shielded outputs
            if (tx.vShieldedOutput.size() > 0)
                return state.DoS(
                    dosLevelPotentiallyRelaxing,
                    error("ContextualCheckTransaction(): coinbase has output descriptions"),
                    REJECT_INVALID, "bad-cb-has-output-description");
        }
    }

    // Rules that apply to Canopy or later:
    if (canopyActive) {
        for (const JSDescription& joinsplit : tx.vJoinSplit) {
            if (joinsplit.vpub_old > 0) {
                return state.DoS(DOS_LEVEL_BLOCK, error("ContextualCheckTransaction(): joinsplit.vpub_old nonzero"),
                                 REJECT_INVALID, "bad-txns-vpub_old-nonzero");
            }
        }

        if (tx.IsCoinBase()) {
            // Detect transparent funding streams.
            for (const CTxOut& output : tx.vout) {
                for (auto it = fundingStreamElements.begin(); it != fundingStreamElements.end(); ++it) {
                    const CScript* taddr = std::get_if<CScript>(&(it->first));
                    if (taddr && output.scriptPubKey == *taddr && output.nValue == it->second) {
                        fundingStreamElements.erase(it);
                        break;
                    }
                }
            }

            if (!fundingStreamElements.empty()) {
                return state.DoS(100, error("ContextualCheckTransaction(): funding stream missing at height %d", nHeight),
                                 REJECT_INVALID, "cb-funding-stream-missing");
            }
        }
    } else {
        // Rules that apply generally before Canopy. These were
        // previously noncontextual checks that became contextual
        // after Canopy activation.
    }

    // Rules that apply to NU5 or later:
    if (nu5Active) {
        // Reject transactions with invalid version group id
        if (!futureActive) {
            if (!(tx.nVersionGroupId == SAPLING_VERSION_GROUP_ID || tx.nVersionGroupId == ZIP225_VERSION_GROUP_ID)) {
                return state.DoS(
                    dosLevelPotentiallyRelaxing,
                    error("ContextualCheckTransaction(): invalid NU5 tx version"),
                    REJECT_INVALID, "bad-nu5-tx-version-group-id");
            }
        }

        // Check that the consensus branch ID is unset in Sapling V4 transactions
        if (tx.nVersionGroupId == SAPLING_VERSION_GROUP_ID) {
            // NOTE: This is an internal zcashd consistency
            // check; it does not correspond to a consensus rule in the
            // protocol specification, but is instead an artifact of the
            // internal zcashd transaction representation.
            if (tx.GetConsensusBranchId()) {
                return state.DoS(
                    dosLevelPotentiallyRelaxing,
                    error("ContextualCheckTransaction(): pre-NU5 transaction has consensus branch id set"),
                    REJECT_INVALID, "bad-tx-has-consensus-branch-id");
            }
        }

        // Reject transactions with invalid version
        if (tx.nVersionGroupId == ZIP225_VERSION_GROUP_ID) {
            if (tx.nVersion < ZIP225_MIN_TX_VERSION) {
                return state.DoS(
                    dosLevelConstricting,
                    error("ContextualCheckTransaction(): ZIP225 version too low"),
                    REJECT_INVALID, "bad-tx-zip225-version-too-low");
            }

            if (tx.nVersion > ZIP225_MAX_TX_VERSION) {
                return state.DoS(
                    dosLevelPotentiallyRelaxing,
                    error("ContextualCheckTransaction(): ZIP225 version too high"),
                    REJECT_INVALID, "bad-tx-zip225-version-too-high");
            }

            // tx.nConsensusBranchId must match the current consensus branch id
            if (!(tx.GetConsensusBranchId() && *tx.GetConsensusBranchId() == consensusBranchId)) {
                return state.DoS(
                    dosLevelPotentiallyRelaxing,
                    error("ContextualCheckTransaction(): transaction's consensus branch id does not match the current consensus branch"),
                    REJECT_INVALID, "bad-tx-consensus-branch-id-mismatch");
            }

            // v5 transactions must have empty joinSplits
            if (!(tx.vJoinSplit.empty())) {
                return state.DoS(
                    dosLevelPotentiallyRelaxing,
                    error("ContextualCheckTransaction(): Sprout JoinSplits not allowed in ZIP225 transactions"),
                    REJECT_INVALID, "bad-tx-has-joinsplits");
            }
        }

        if (tx.IsCoinBase()) {
            if (!orchard_bundle.CoinbaseOutputsAreValid()) {
                return state.DoS(
                    DOS_LEVEL_BLOCK,
                    error("ContextualCheckTransaction(): Orchard coinbase action has invalid ciphertext"),
                    REJECT_INVALID, "bad-cb-action-invalid-ciphertext");
            }
        } else {
            // ZIP 203: From NU5, the upper limit on nExpiryHeight is removed for coinbase
            // transactions.
            if (tx.nExpiryHeight >= TX_EXPIRY_HEIGHT_THRESHOLD) {
                return state.DoS(100, error("CheckTransaction(): expiry height is too high"),
                                 REJECT_INVALID, "bad-tx-expiry-height-too-high");
            }
        }
    } else {
        // Rules that apply generally before NU5. These were previously
        // noncontextual checks that became contextual after NU5 activation.

        if (tx.nExpiryHeight >= TX_EXPIRY_HEIGHT_THRESHOLD) {
            return state.DoS(
                dosLevelPotentiallyRelaxing,
                error("CheckTransaction(): expiry height is too high"),
                REJECT_INVALID, "bad-tx-expiry-height-too-high");
        }

        // Check that Orchard transaction components are not present prior to
        // NU5. NOTE: This is an internal zcashd consistency check; it does not
        // correspond to a consensus rule in the protocol specification, but is
        // instead an artifact of the internal zcashd transaction
        // representation.
        if (orchard_bundle.IsPresent()) {
            return state.DoS(
                dosLevelPotentiallyRelaxing,
                error("ContextualCheckTransaction(): pre-NU5 transaction has Orchard actions"),
                REJECT_INVALID, "bad-tx-has-orchard-actions");
        }

        // Check that the consensus branch ID is unset prior to NU5. NOTE: This
        // is an internal zcashd consistency check; it does not correspond to a
        // consensus rule in the protocol specification, but is instead an
        // artifact of the internal zcashd transaction representation.
        if (tx.GetConsensusBranchId()) {
            return state.DoS(
                dosLevelPotentiallyRelaxing,
                error("ContextualCheckTransaction(): pre-NU5 transaction has consensus branch id set"),
                REJECT_INVALID, "bad-tx-has-consensus-branch-id");
        }
    }

    // Rules that apply to the future epoch
    if (futureActive) {
        switch (tx.nVersionGroupId) {
            case ZFUTURE_VERSION_GROUP_ID:
                if (tx.nVersion <= ZIP225_MAX_TX_VERSION) {
                    return state.DoS(
                        dosLevelConstricting,
                        error("ContextualCheckTransaction(): Future version too low"),
                        REJECT_INVALID, "bad-tx-zfuture-version-too-low");
                }

                // Reject transactions with invalid version
                if (tx.nVersion > ZIP225_MAX_TX_VERSION + 1) {
                    return state.DoS(
                        dosLevelPotentiallyRelaxing,
                        error("ContextualCheckTransaction(): Future version too high"),
                        REJECT_INVALID, "bad-tx-zfuture-version-too-high");
                }
                break;
            case SAPLING_VERSION_GROUP_ID:
                // Allow V4 transactions while futureActive
                break;
            case ZIP225_VERSION_GROUP_ID:
                // Allow V5 transactions while futureActive
                break;
            default:
                return state.DoS(
                    dosLevelPotentiallyRelaxing,
                    error("ContextualCheckTransaction(): invalid future tx version group id"),
                    REJECT_INVALID, "bad-zfuture-tx-version-group-id");
        }
    } else {
        // Rules that apply generally before the next release epoch
    }

    auto prevConsensusBranchId = PrevEpochBranchId(consensusBranchId, consensus);
    uint256 dataToBeSigned;
    uint256 prevDataToBeSigned;

    // Create signature hashes for shielded components.
    // Orchard signatures are checked in CheckTransaction, so we
    // don't need to take Orchard into account here.
    if (!tx.vJoinSplit.empty() ||
        !tx.vShieldedSpend.empty() ||
        !tx.vShieldedOutput.empty())
    {
        // Empty output script.
        CScript scriptCode;
        try {
            dataToBeSigned = SignatureHash(scriptCode, tx, NOT_AN_INPUT, SIGHASH_ALL, 0, consensusBranchId);
            prevDataToBeSigned = SignatureHash(scriptCode, tx, NOT_AN_INPUT, SIGHASH_ALL, 0, prevConsensusBranchId);
        } catch (std::logic_error ex) {
            // A logic error should never occur because we pass NOT_AN_INPUT and
            // SIGHASH_ALL to SignatureHash().
            return state.DoS(100, error("ContextualCheckTransaction(): error computing signature hash"),
                             REJECT_INVALID, "error-computing-signature-hash");
        }
    }

    if (!tx.vJoinSplit.empty())
    {
        if (!ed25519_verify(&tx.joinSplitPubKey, &tx.joinSplitSig, dataToBeSigned.begin(), 32)) {
            // Check whether the failure was caused by an outdated consensus
            // branch ID; if so, inform the node that they need to upgrade. We
            // only check the previous epoch's branch ID, on the assumption that
            // users creating transactions will notice their transactions
            // failing before a second network upgrade occurs.
            if (ed25519_verify(&tx.joinSplitPubKey,
                               &tx.joinSplitSig,
                               prevDataToBeSigned.begin(), 32)) {
                return state.DoS(
                    dosLevelPotentiallyRelaxing, false, REJECT_INVALID, strprintf(
                        "old-consensus-branch-id (Expected %s, found %s)",
                        HexInt(consensusBranchId),
                        HexInt(prevConsensusBranchId)));
            }
            return state.DoS(
                dosLevelPotentiallyRelaxing,
                error("ContextualCheckTransaction(): invalid joinsplit signature"),
                REJECT_INVALID, "bad-txns-invalid-joinsplit-signature");
        }
    }

    if (!tx.vShieldedSpend.empty() ||
        !tx.vShieldedOutput.empty())
    {
        // The nu5Active flag passed in here enables the new consensus rules from ZIP 216
        // (https://zips.z.cash/zip-0216#specification) on the following fields:
        //
        // - spendAuthSig in Sapling Spend descriptions
        // - bindingSigSapling
        auto ctx = librustzcash_sapling_verification_ctx_init(nu5Active);

        for (const SpendDescription &spend : tx.vShieldedSpend) {
            if (!librustzcash_sapling_check_spend(
                ctx,
                spend.cv.begin(),
                spend.anchor.begin(),
                spend.nullifier.begin(),
                spend.rk.begin(),
                spend.zkproof.begin(),
                spend.spendAuthSig.begin(),
                dataToBeSigned.begin()
            ))
            {
                librustzcash_sapling_verification_ctx_free(ctx);
                return state.DoS(
                    dosLevelPotentiallyRelaxing,
                    error("ContextualCheckTransaction(): Sapling spend description invalid"),
                    REJECT_INVALID, "bad-txns-sapling-spend-description-invalid");
            }
        }

        for (const OutputDescription &output : tx.vShieldedOutput) {
            if (!librustzcash_sapling_check_output(
                ctx,
                output.cv.begin(),
                output.cmu.begin(),
                output.ephemeralKey.begin(),
                output.zkproof.begin()
            ))
            {
                librustzcash_sapling_verification_ctx_free(ctx);
                // This should be a non-contextual check, but we check it here
                // as we need to pass over the outputs anyway in order to then
                // call librustzcash_sapling_final_check().
                return state.DoS(100, error("ContextualCheckTransaction(): Sapling output description invalid"),
                                      REJECT_INVALID, "bad-txns-sapling-output-description-invalid");
            }
        }

        if (!librustzcash_sapling_final_check(
            ctx,
            tx.GetValueBalanceSapling(),
            tx.bindingSig.begin(),
            dataToBeSigned.begin()
        ))
        {
            librustzcash_sapling_verification_ctx_free(ctx);
            return state.DoS(
                dosLevelPotentiallyRelaxing,
                error("ContextualCheckTransaction(): Sapling binding signature invalid"),
                REJECT_INVALID, "bad-txns-sapling-binding-signature-invalid");
        }

        librustzcash_sapling_verification_ctx_free(ctx);
    }

    return true;
}


bool CheckTransaction(const CTransaction& tx, CValidationState &state,
                      ProofVerifier& verifier, orchard::AuthValidator& orchardAuth)
{
    // Don't count coinbase transactions because mining skews the count
    if (!tx.IsCoinBase()) {
        transactionsValidated.increment();
    }

    if (!CheckTransactionWithoutProofVerification(tx, state)) {
        return false;
    } else {
        // Ensure that zk-SNARKs verify
        for (const JSDescription &joinsplit : tx.vJoinSplit) {
            if (!verifier.VerifySprout(joinsplit, tx.joinSplitPubKey)) {
                return state.DoS(100, error("CheckTransaction(): joinsplit does not verify"),
                                    REJECT_INVALID, "bad-txns-joinsplit-verification-failed");
            }
        }

        // Sapling zk-SNARK proofs are checked in librustzcash_sapling_check_{spend,output},
        // called from ContextualCheckTransaction.

        // Check bundle-specific Orchard consensus rules. Since we check encoding
        // consensus rules at parse time, and signature validation is batched, all we are
        // checking here is proof validity. Once we implement batched proof verification,
        // this will move into orchardAuth.
        auto orchardBundle = tx.GetOrchardBundle();
        if (!orchardBundle.CheckBundleSpecificConsensusRules()) {
            return state.DoS(
                100, error("CheckTransaction(): Orchard bundle proof does not verify"),
                REJECT_INVALID, "bad-txns-orchard-verification-failed");
        }

        // Queue Orchard signatures to be batch-validated.
        orchardBundle.QueueSignatureValidation(orchardAuth, tx.GetHash());

        return true;
    }
}

/**
 * Basic checks that don't depend on any context.
 *
 * This function must obey the following contract: it must reject transactions
 * that are invalid according to the transaction's embedded version
 * information, but it may accept transactions that are valid with respect to
 * embedded version information but are invalid with respect to current
 * consensus rules.
 */
bool CheckTransactionWithoutProofVerification(const CTransaction& tx, CValidationState &state)
{
    /**
     * Previously:
     * 1. The consensus rule below was:
     *        if (tx.nVersion < SPROUT_MIN_TX_VERSION) { ... }
     *    which checked if tx.nVersion fell within the range:
     *        INT32_MIN <= tx.nVersion < SPROUT_MIN_TX_VERSION
     * 2. The parser allowed tx.nVersion to be negative
     *
     * Now:
     * 1. The consensus rule checks to see if tx.Version falls within the range:
     *        0 <= tx.nVersion < SPROUT_MIN_TX_VERSION
     * 2. The previous consensus rule checked for negative values within the range:
     *        INT32_MIN <= tx.nVersion < 0
     *    This is unnecessary for Overwinter transactions since the parser now
     *    interprets the sign bit as fOverwintered, so tx.nVersion is always >=0,
     *    and when Overwinter is not active ContextualCheckTransaction rejects
     *    transactions with fOverwintered set.  When fOverwintered is set,
     *    this function and ContextualCheckTransaction will together check to
     *    ensure tx.nVersion avoids the following ranges:
     *        0 <= tx.nVersion < OVERWINTER_MIN_TX_VERSION
     *        OVERWINTER_MAX_TX_VERSION < tx.nVersion <= INT32_MAX
     */
    if (!tx.fOverwintered && tx.nVersion < SPROUT_MIN_TX_VERSION) {
        return state.DoS(100, error("CheckTransaction(): version too low"),
                         REJECT_INVALID, "bad-txns-version-too-low");
    }
    else if (tx.fOverwintered) {
        if (tx.nVersion < OVERWINTER_MIN_TX_VERSION) {
            return state.DoS(100, error("CheckTransaction(): overwinter version too low"),
                REJECT_INVALID, "bad-tx-overwinter-version-too-low");
        }
        if (tx.nVersionGroupId != OVERWINTER_VERSION_GROUP_ID &&
                tx.nVersionGroupId != SAPLING_VERSION_GROUP_ID &&
                tx.nVersionGroupId != ZIP225_VERSION_GROUP_ID &&
                tx.nVersionGroupId != ZFUTURE_VERSION_GROUP_ID) {
            return state.DoS(100, error("CheckTransaction(): unknown tx version group id"),
                    REJECT_INVALID, "bad-tx-version-group-id");
        }
    }
    auto orchard_bundle = tx.GetOrchardBundle();

    // Transactions must contain some potential source of funds. This rejects
    // obviously-invalid transaction constructions early, but cannot prevent
    // e.g. a pure Sapling transaction with only dummy spends (which is
    // undetectable). Contextual checks ensure that only one of Sprout
    // joinsplits or Orchard actions may be present.
    // Note that orchard_bundle.SpendsEnabled() is false when no
    // Orchard bundle is present, i.e. when nActionsOrchard == 0.
    if (tx.vin.empty() &&
        tx.vJoinSplit.empty() &&
        tx.vShieldedSpend.empty() &&
        !orchard_bundle.SpendsEnabled())
    {
        return state.DoS(10, error("CheckTransaction(): no source of funds"),
                         REJECT_INVALID, "bad-txns-no-source-of-funds");
    }
    // Transactions must contain some potential useful sink of funds.  This
    // rejects obviously-invalid transaction constructions early, but cannot
    // prevent e.g. a pure Sapling transaction with only dummy outputs (which
    // is undetectable), and does not prevent transparent transactions from
    // sending all funds to miners.  Contextual checks ensure that only one of
    // Sprout joinsplits or Orchard actions may be present.
    // Note that orchard_bundle.OutputsEnabled() is false when no
    // Orchard bundle is present, i.e. when nActionsOrchard == 0.
    if (tx.vout.empty() &&
        tx.vJoinSplit.empty() &&
        tx.vShieldedOutput.empty() &&
        !orchard_bundle.OutputsEnabled())
    {
        return state.DoS(10, error("CheckTransaction(): no sink of funds"),
                         REJECT_INVALID, "bad-txns-no-sink-of-funds");
    }

    // Size limits
    static_assert(MAX_BLOCK_SIZE >= MAX_TX_SIZE_AFTER_SAPLING); // sanity
    static_assert(MAX_TX_SIZE_AFTER_SAPLING > MAX_TX_SIZE_BEFORE_SAPLING); // sanity
    if (::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION) > MAX_TX_SIZE_AFTER_SAPLING)
        return state.DoS(100, error("CheckTransaction(): size limits failed"),
                         REJECT_INVALID, "bad-txns-oversize");

    // Check for negative or overflow output values
    CAmount nValueOut = 0;
    for (const CTxOut& txout : tx.vout)
    {
        if (txout.nValue < 0)
            return state.DoS(100, error("CheckTransaction(): txout.nValue negative"),
                             REJECT_INVALID, "bad-txns-vout-negative");
        if (txout.nValue > MAX_MONEY)
            return state.DoS(100, error("CheckTransaction(): txout.nValue too high"),
                             REJECT_INVALID, "bad-txns-vout-toolarge");
        nValueOut += txout.nValue;
        if (!MoneyRange(nValueOut))
            return state.DoS(100, error("CheckTransaction(): txout total out of range"),
                             REJECT_INVALID, "bad-txns-txouttotal-toolarge");
    }

    // Check for non-zero valueBalanceSapling when there are no Sapling inputs or outputs
    if (tx.vShieldedSpend.empty() && tx.vShieldedOutput.empty() && tx.GetValueBalanceSapling() != 0) {
        return state.DoS(100, error("CheckTransaction(): tx.valueBalanceSapling has no sources or sinks"),
                            REJECT_INVALID, "bad-txns-valuebalance-nonzero");
    }

    // Check for overflow valueBalanceSapling
    if (tx.GetValueBalanceSapling() > MAX_MONEY || tx.GetValueBalanceSapling() < -MAX_MONEY) {
        return state.DoS(100, error("CheckTransaction(): abs(tx.valueBalanceSapling) too large"),
                            REJECT_INVALID, "bad-txns-valuebalance-toolarge");
    }

    if (tx.GetValueBalanceSapling() <= 0) {
        // NB: negative valueBalanceSapling "takes" money from the transparent value pool just as outputs do
        nValueOut += -tx.GetValueBalanceSapling();

        if (!MoneyRange(nValueOut)) {
            return state.DoS(100, error("CheckTransaction(): txout total out of range"),
                                REJECT_INVALID, "bad-txns-txouttotal-toolarge");
        }
    }

    // nSpendsSapling, nOutputsSapling, and nActionsOrchard MUST all be less than 2^16
    size_t max_elements = (1 << 16) - 1;
    if (tx.vShieldedSpend.size() > max_elements) {
        return state.DoS(
            100,
            error("CheckTransaction(): 2^16 or more Sapling spends"),
            REJECT_INVALID, "bad-tx-too-many-sapling-spends");
    }
    if (tx.vShieldedOutput.size() > max_elements) {
        return state.DoS(
            100,
            error("CheckTransaction(): 2^16 or more Sapling outputs"),
            REJECT_INVALID, "bad-tx-too-many-sapling-outputs");
    }
    if (orchard_bundle.GetNumActions() > max_elements) {
        return state.DoS(
            100,
            error("CheckTransaction(): 2^16 or more Orchard actions"),
            REJECT_INVALID, "bad-tx-too-many-orchard-actions");
    }

    // Check that if neither Orchard spends nor outputs are enabled, the transaction contains
    // no Orchard actions. This subsumes the check that valueBalanceOrchard must equal zero
    // in the case that both spends and outputs are disabled.
    if (orchard_bundle.GetNumActions() > 0 && !orchard_bundle.OutputsEnabled() && !orchard_bundle.SpendsEnabled()) {
        return state.DoS(
            100,
            error("CheckTransaction(): Orchard actions are present, but flags do not permit Orchard spends or outputs"),
            REJECT_INVALID, "bad-tx-orchard-flags-disable-actions");
    }

    auto valueBalanceOrchard = orchard_bundle.GetValueBalance();

    // Check for overflow valueBalanceOrchard
    if (valueBalanceOrchard > MAX_MONEY || valueBalanceOrchard < -MAX_MONEY) {
        return state.DoS(100, error("CheckTransaction(): abs(tx.valueBalanceOrchard) too large"),
                         REJECT_INVALID, "bad-txns-valuebalance-toolarge");
    }

    if (valueBalanceOrchard <= 0) {
        // NB: negative valueBalanceOrchard "takes" money from the transparent value pool just as outputs do
        nValueOut += -valueBalanceOrchard;

        if (!MoneyRange(nValueOut)) {
            return state.DoS(100, error("CheckTransaction(): txout total out of range"),
                             REJECT_INVALID, "bad-txns-txouttotal-toolarge");
        }
    }

    // Ensure that joinsplit values are well-formed
    for (const JSDescription& joinsplit : tx.vJoinSplit)
    {
        if (joinsplit.vpub_old < 0) {
            return state.DoS(100, error("CheckTransaction(): joinsplit.vpub_old negative"),
                             REJECT_INVALID, "bad-txns-vpub_old-negative");
        }

        if (joinsplit.vpub_new < 0) {
            return state.DoS(100, error("CheckTransaction(): joinsplit.vpub_new negative"),
                             REJECT_INVALID, "bad-txns-vpub_new-negative");
        }

        if (joinsplit.vpub_old > MAX_MONEY) {
            return state.DoS(100, error("CheckTransaction(): joinsplit.vpub_old too high"),
                             REJECT_INVALID, "bad-txns-vpub_old-toolarge");
        }

        if (joinsplit.vpub_new > MAX_MONEY) {
            return state.DoS(100, error("CheckTransaction(): joinsplit.vpub_new too high"),
                             REJECT_INVALID, "bad-txns-vpub_new-toolarge");
        }

        if (joinsplit.vpub_new != 0 && joinsplit.vpub_old != 0) {
            return state.DoS(100, error("CheckTransaction(): joinsplit.vpub_new and joinsplit.vpub_old both nonzero"),
                             REJECT_INVALID, "bad-txns-vpubs-both-nonzero");
        }

        nValueOut += joinsplit.vpub_old;
        if (!MoneyRange(nValueOut)) {
            return state.DoS(100, error("CheckTransaction(): txout total out of range"),
                             REJECT_INVALID, "bad-txns-txouttotal-toolarge");
        }
    }

    // Ensure input values do not exceed MAX_MONEY
    // We have not resolved the txin values at this stage,
    // but we do know what the joinsplits claim to add
    // to the value pool.
    {
        CAmount nValueIn = 0;
        for (std::vector<JSDescription>::const_iterator it(tx.vJoinSplit.begin()); it != tx.vJoinSplit.end(); ++it)
        {
            nValueIn += it->vpub_new;

            if (!MoneyRange(it->vpub_new) || !MoneyRange(nValueIn)) {
                return state.DoS(100, error("CheckTransaction(): txin total out of range"),
                                 REJECT_INVALID, "bad-txns-txintotal-toolarge");
            }
        }

        // Also check for Sapling
        if (tx.GetValueBalanceSapling() >= 0) {
            // NB: positive valueBalanceSapling "adds" money to the transparent value pool, just as inputs do
            nValueIn += tx.GetValueBalanceSapling();

            if (!MoneyRange(nValueIn)) {
                return state.DoS(100, error("CheckTransaction(): txin total out of range"),
                                 REJECT_INVALID, "bad-txns-txintotal-toolarge");
            }
        }

        // Also check for Orchard
        if (valueBalanceOrchard >= 0) {
            // NB: positive valueBalanceOrchard "adds" money to the transparent value pool, just as inputs do
            nValueIn += valueBalanceOrchard;

            if (!MoneyRange(nValueIn)) {
                return state.DoS(100, error("CheckTransaction(): txin total out of range"),
                                    REJECT_INVALID, "bad-txns-txintotal-toolarge");
            }
        }
    }

    // Check for duplicate inputs
    set<COutPoint> vInOutPoints;
    for (const CTxIn& txin : tx.vin)
    {
        if (vInOutPoints.count(txin.prevout))
            return state.DoS(100, error("CheckTransaction(): duplicate inputs"),
                             REJECT_INVALID, "bad-txns-inputs-duplicate");
        vInOutPoints.insert(txin.prevout);
    }

    // Check for duplicate joinsplit nullifiers in this transaction
    {
        set<uint256> vJoinSplitNullifiers;
        for (const JSDescription& joinsplit : tx.vJoinSplit)
        {
            for (const uint256& nf : joinsplit.nullifiers)
            {
                if (vJoinSplitNullifiers.count(nf))
                    return state.DoS(100, error("CheckTransaction(): duplicate nullifiers"),
                                REJECT_INVALID, "bad-joinsplits-nullifiers-duplicate");

                vJoinSplitNullifiers.insert(nf);
            }
        }
    }

    // Check for duplicate sapling nullifiers in this transaction
    {
        set<uint256> vSaplingNullifiers;
        for (const SpendDescription& spend_desc : tx.vShieldedSpend)
        {
            if (vSaplingNullifiers.count(spend_desc.nullifier))
                return state.DoS(100, error("CheckTransaction(): duplicate nullifiers"),
                            REJECT_INVALID, "bad-spend-description-nullifiers-duplicate");

            vSaplingNullifiers.insert(spend_desc.nullifier);
        }
    }

    // Check for duplicate orchard nullifiers in this transaction
    {
        std::set<uint256> vOrchardNullifiers;
        for (const uint256& nf : tx.GetOrchardBundle().GetNullifiers())
        {
            if (vOrchardNullifiers.count(nf))
                return state.DoS(100, error("CheckTransaction(): duplicate nullifiers"),
                            REJECT_INVALID, "bad-orchard-nullifiers-duplicate");

            vOrchardNullifiers.insert(nf);
        }
    }

    if (tx.IsCoinBase())
    {
        // There should be no joinsplits in a coinbase transaction
        if (tx.vJoinSplit.size() > 0)
            return state.DoS(100, error("CheckTransaction(): coinbase has joinsplits"),
                             REJECT_INVALID, "bad-cb-has-joinsplits");

        // A coinbase transaction cannot have spend descriptions
        if (tx.vShieldedSpend.size() > 0)
            return state.DoS(100, error("CheckTransaction(): coinbase has spend descriptions"),
                             REJECT_INVALID, "bad-cb-has-spend-description");
        // See ContextualCheckTransaction for consensus rules on coinbase output descriptions.
        if (orchard_bundle.SpendsEnabled())
            return state.DoS(100, error("CheckTransaction(): coinbase has enableSpendsOrchard set"),
                             REJECT_INVALID, "bad-cb-has-orchard-spend");

        if (tx.vin[0].scriptSig.size() < 2 || tx.vin[0].scriptSig.size() > 100)
            return state.DoS(100, error("CheckTransaction(): coinbase script size"),
                             REJECT_INVALID, "bad-cb-length");
    }
    else
    {
        for (const CTxIn& txin : tx.vin)
            if (txin.prevout.IsNull())
                return state.DoS(10, error("CheckTransaction(): prevout is null"),
                                 REJECT_INVALID, "bad-txns-prevout-null");
    }

    return true;
}

CAmount GetMinRelayFee(const CTransaction& tx, const CTxMemPool& pool, unsigned int nBytes, bool fAllowFree)
{
    uint256 hash = tx.GetHash();
    double dPriorityDelta = 0;
    CAmount nFeeDelta = 0;
    pool.ApplyDeltas(hash, dPriorityDelta, nFeeDelta);
    if (dPriorityDelta > 0 || nFeeDelta > 0)
        return 0;

    CAmount nMinFee = ::minRelayTxFee.GetFeeForRelay(nBytes);

    if (fAllowFree)
    {
        // There is a free transaction area in blocks created by most miners,
        // * If we are relaying we allow transactions up to DEFAULT_BLOCK_PRIORITY_SIZE - 1000
        //   to be considered to fall into this category. We don't want to encourage sending
        //   multiple transactions instead of one big transaction to avoid fees.
        if (nBytes < (DEFAULT_BLOCK_PRIORITY_SIZE - 1000))
            nMinFee = 0;
    }

    if (!MoneyRange(nMinFee))
        nMinFee = MAX_MONEY;
    return nMinFee;
}


bool AcceptToMemoryPool(
        const CChainParams& chainparams,
        CTxMemPool& pool, CValidationState &state, const CTransaction &tx, bool fLimitFree,
        bool* pfMissingInputs, bool fRejectAbsurdFee)
{
    AssertLockHeld(cs_main);
    LOCK(pool.cs); // mempool "read lock" (held through pool.addUnchecked())
    if (pfMissingInputs) {
        *pfMissingInputs = false;
    }

    int nextBlockHeight = chainActive.Height() + 1;

    // Grab the branch ID we expect this transaction to commit to.
    auto consensusBranchId = CurrentEpochBranchId(nextBlockHeight, chainparams.GetConsensus());

    if (pool.IsRecentlyEvicted(tx.GetHash())) {
        LogPrint("mempool", "Dropping txid %s : recently evicted", tx.GetHash().ToString());
        return false;
    }

    // This will be a single-transaction batch, which is still more efficient as every
    // Orchard bundle contains at least two signatures.
    auto orchardAuth = orchard::AuthValidator::Batch();

    auto verifier = ProofVerifier::Strict();
    if (!CheckTransaction(tx, state, verifier, orchardAuth))
        return error("AcceptToMemoryPool: CheckTransaction failed");

    // Check transaction contextually against the set of consensus rules which apply in the next block to be mined.
    if (!ContextualCheckTransaction(tx, state, chainparams, nextBlockHeight, false)) {
        return error("AcceptToMemoryPool: ContextualCheckTransaction failed");
    }

    // DoS mitigation: reject transactions expiring soon
    // Note that if a valid transaction belonging to the wallet is in the mempool and the node is shutdown,
    // upon restart, CWalletTx::AcceptToMemoryPool() will be invoked which might result in rejection.
    if (IsExpiringSoonTx(tx, nextBlockHeight)) {
        return state.DoS(0, error("AcceptToMemoryPool(): transaction is expiring soon"), REJECT_INVALID, "tx-expiring-soon");
    }

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return state.DoS(100, error("AcceptToMemoryPool: coinbase as individual tx"),
                         REJECT_INVALID, "coinbase");

    // Rather not work on nonstandard transactions (unless -testnet/-regtest)
    string reason;
    if (chainparams.RequireStandard() && !IsStandardTx(tx, reason, chainparams, nextBlockHeight))
        return state.DoS(0,
                         error("AcceptToMemoryPool: nonstandard transaction: %s", reason),
                         REJECT_NONSTANDARD, reason);

    // Only accept nLockTime-using transactions that can be mined in the next
    // block; we don't want our mempool filled up with transactions that can't
    // be mined yet.
    if (!CheckFinalTx(tx, STANDARD_LOCKTIME_VERIFY_FLAGS))
        return state.DoS(0, false, REJECT_NONSTANDARD, "non-final");

    // is it already in the memory pool?
    uint256 hash = tx.GetHash();
    if (pool.exists(hash))
        return false;

    // Check for conflicts with in-memory transactions
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        COutPoint outpoint = tx.vin[i].prevout;
        if (pool.mapNextTx.count(outpoint))
        {
            // Disable replacement feature for now
            return false;
        }
    }
    for (const JSDescription &joinsplit : tx.vJoinSplit) {
        for (const uint256 &nf : joinsplit.nullifiers) {
            if (pool.nullifierExists(nf, SPROUT)) {
                return false;
            }
        }
    }
    for (const SpendDescription &spendDescription : tx.vShieldedSpend) {
        if (pool.nullifierExists(spendDescription.nullifier, SAPLING)) {
            return false;
        }
    }

    {
        CCoinsView dummy;
        CCoinsViewCache view(&dummy);

        CAmount nValueIn = 0;
        CCoinsViewMemPool viewMemPool(pcoinsTip, pool);
        view.SetBackend(viewMemPool);

        // do we already have it?
        if (view.HaveCoins(hash))
            return false;

        // do all inputs exist?
        // Note that this does not check for the presence of actual outputs (see the next check for that),
        // and only helps with filling in pfMissingInputs (to determine missing vs spent).
        for (const CTxIn txin : tx.vin) {
            if (!view.HaveCoins(txin.prevout.hash)) {
                if (pfMissingInputs)
                    *pfMissingInputs = true;
                return false;
            }
        }

        // are the actual inputs available?
        if (!view.HaveInputs(tx))
            return state.Invalid(error("AcceptToMemoryPool: inputs already spent"),
                                 REJECT_DUPLICATE, "bad-txns-inputs-spent");

        // Are the shielded spends' requirements met?
        auto unmetShieldedReq = view.HaveShieldedRequirements(tx);
        if (unmetShieldedReq) {
            auto txid = tx.GetHash().ToString();
            auto rejectCode = ShieldedReqRejectCode(*unmetShieldedReq);
            auto rejectReason = ShieldedReqRejectReason(*unmetShieldedReq);
            TracingError(
                "main", "AcceptToMemoryPool(): shielded requirements not met",
                "txid", txid.c_str(),
                "reason", rejectReason.c_str());
            return state.Invalid(false, rejectCode, rejectReason);
        }

        // Bring the best block into scope
        view.GetBestBlock();

        nValueIn = view.GetValueIn(tx);

        view.SetBackend(dummy);

        // Check for non-standard pay-to-script-hash in inputs
        if (chainparams.RequireStandard() && !AreInputsStandard(tx, view, consensusBranchId))
            return error("AcceptToMemoryPool: nonstandard transaction input");

        // Check that the transaction doesn't have an excessive number of
        // sigops, making it impossible to mine. Since the coinbase transaction
        // itself can contain sigops MAX_STANDARD_TX_SIGOPS is less than
        // MAX_BLOCK_SIGOPS; we still consider this an invalid rather than
        // merely non-standard transaction.
        unsigned int nSigOps = GetLegacySigOpCount(tx);
        nSigOps += GetP2SHSigOpCount(tx, view);
        if (nSigOps > MAX_STANDARD_TX_SIGOPS)
            return state.DoS(0,
                             error("AcceptToMemoryPool: too many sigops %s, %d > %d",
                                   hash.ToString(), nSigOps, MAX_STANDARD_TX_SIGOPS),
                             REJECT_NONSTANDARD, "bad-txns-too-many-sigops");

        CAmount nValueOut = tx.GetValueOut();
        CAmount nFees = nValueIn-nValueOut;
        double dPriority = view.GetPriority(tx, chainActive.Height());

        // Keep track of transactions that spend a coinbase, which we re-scan
        // during reorgs to ensure COINBASE_MATURITY is still met.
        bool fSpendsCoinbase = false;
        for (const CTxIn &txin : tx.vin) {
            const CCoins *coins = view.AccessCoins(txin.prevout.hash);
            if (coins->IsCoinBase()) {
                fSpendsCoinbase = true;
                break;
            }
        }

        // For v1-v4 transactions, we don't yet know if the transaction commits
        // to consensusBranchId, but if the entry gets added to the mempool, then
        // it has passed ContextualCheckInputs and therefore this is correct.
        CTxMemPoolEntry entry(tx, nFees, GetTime(), dPriority, chainActive.Height(), pool.HasNoInputsOf(tx), fSpendsCoinbase, nSigOps, consensusBranchId);
        unsigned int nSize = entry.GetTxSize();

        // Before zcashd 4.2.0, we had a condition here to always accept a tx if it contained
        // JoinSplits and had at least the default fee. It is no longer necessary to treat
        // that as a special case, because the fee returned by GetMinRelayFee is always at
        // most DEFAULT_FEE.

        CAmount txMinFee = GetMinRelayFee(tx, pool, nSize, true);
        if (fLimitFree && nFees < txMinFee) {
            return state.DoS(0, error("AcceptToMemoryPool: not enough fees %s, %d < %d",
                                      hash.ToString(), nFees, txMinFee),
                             REJECT_INSUFFICIENTFEE, "insufficient fee");
        }

        // Require that free transactions have sufficient priority to be mined in the next block.
        if (GetBoolArg("-relaypriority", DEFAULT_RELAYPRIORITY) && nFees < ::minRelayTxFee.GetFeeForRelay(nSize) && !AllowFree(view.GetPriority(tx, chainActive.Height() + 1))) {
            return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "insufficient priority");
        }

        // Continuously rate-limit free (really, very-low-fee) transactions
        // This mitigates 'penny-flooding' -- sending thousands of free transactions just to
        // be annoying or make others' transactions take longer to confirm.
        if (fLimitFree && nFees < ::minRelayTxFee.GetFeeForRelay(nSize))
        {
            static CCriticalSection csFreeLimiter;
            static double dFreeCount;
            static int64_t nLastTime;
            int64_t nNow = GetTime();

            LOCK(csFreeLimiter);

            // Use an exponentially decaying ~10-minute window:
            dFreeCount *= pow(1.0 - 1.0/600.0, (double)(nNow - nLastTime));
            nLastTime = nNow;
            // -limitfreerelay unit is thousand-bytes-per-minute
            // At default rate it would take over a month to fill 1GB
            if (dFreeCount >= GetArg("-limitfreerelay", DEFAULT_LIMITFREERELAY) * 10 * 1000)
                return state.DoS(0, error("AcceptToMemoryPool: free transaction rejected by rate limiter"),
                                 REJECT_INSUFFICIENTFEE, "rate limited free transaction");
            LogPrint("mempool", "Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount+nSize);
            dFreeCount += nSize;
        }

        if (fRejectAbsurdFee && nFees > maxTxFee) {
            string errmsg = strprintf("absurdly high fees %s, %d > %d",
                                      hash.ToString(),
                                      nFees, maxTxFee);
            LogPrint("mempool", errmsg.c_str());
            return state.Error("AcceptToMemoryPool: " + errmsg);
        }

        // Check Orchard bundle authorizations.
        // This is done near the end to help prevent CPU exhaustion
        // denial-of-service attacks.
        if (!orchardAuth.Validate()) {
            return state.DoS(100, false, REJECT_INVALID, "bad-orchard-bundle-authorization");
        }

        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        PrecomputedTransactionData txdata(tx);
        if (!ContextualCheckInputs(tx, state, view, true, STANDARD_SCRIPT_VERIFY_FLAGS, true, txdata, chainparams.GetConsensus(), consensusBranchId))
        {
            return error("AcceptToMemoryPool: ConnectInputs failed %s", hash.ToString());
        }

        // Check again against just the consensus-critical mandatory script
        // verification flags, in case of bugs in the standard flags that cause
        // transactions to pass as valid when they're actually invalid. For
        // instance the STRICTENC flag was incorrectly allowing certain
        // CHECKSIG NOT scripts to pass, even though they were invalid.
        //
        // There is a similar check in CreateNewBlock() to prevent creating
        // invalid blocks, however allowing such transactions into the mempool
        // can be exploited as a DoS attack.
        if (!ContextualCheckInputs(tx, state, view, true, MANDATORY_SCRIPT_VERIFY_FLAGS, true, txdata, chainparams.GetConsensus(), consensusBranchId))
        {
            return error("AcceptToMemoryPool: BUG! PLEASE REPORT THIS! ConnectInputs failed against MANDATORY but not STANDARD flags %s", hash.ToString());
        }

        {
            // Store transaction in memory
            pool.addUnchecked(hash, entry, !IsInitialBlockDownload(chainparams.GetConsensus()));

            // Add memory address index
            if (fAddressIndex) {
                pool.addAddressIndex(entry, view);
            }

            // insightexplorer: Add memory spent index
            if (fSpentIndex) {
                pool.addSpentIndex(entry, view);
            }

            pool.EnsureSizeLimit();

            MetricsGauge("zcash.mempool.size.transactions", pool.size());
            MetricsGauge("zcash.mempool.size.bytes", pool.GetTotalTxSize());
            MetricsGauge("zcash.mempool.usage.bytes", pool.DynamicMemoryUsage());
        }
    }

    auto txid = tx.GetHash().ToString();
    auto poolsz = tfm::format("%u", pool.mapTx.size());

    TracingInfo("mempool", "Accepted",
        "txid", txid.c_str(),
        "poolsize", poolsz.c_str());

    return true;
}

bool GetTimestampIndex(unsigned int high, unsigned int low, bool fActiveOnly,
    std::vector<std::pair<uint256, unsigned int> > &hashes)
{
    if (!fTimestampIndex)
        return error("Timestamp index not enabled");

    if (!pblocktree->ReadTimestampIndex(high, low, fActiveOnly, hashes))
        return error("Unable to get hashes for timestamps");

    return true;
}

bool GetSpentIndex(CSpentIndexKey &key, CSpentIndexValue &value)
{
    AssertLockHeld(cs_main);
    if (!fSpentIndex)
        return error("Spent index not enabled");

    if (mempool.getSpentIndex(key, value))
        return true;

    if (!pblocktree->ReadSpentIndex(key, value))
        return error("Unable to get spent index information");

    return true;
}

bool GetAddressIndex(const uint160& addressHash, int type,
                     std::vector<CAddressIndexDbEntry>& addressIndex,
                     int start, int end)
{
    if (!fAddressIndex)
        return error("address index not enabled");

    if (!pblocktree->ReadAddressIndex(addressHash, type, addressIndex, start, end))
        return error("unable to get txids for address");

    return true;
}

bool GetAddressUnspent(const uint160& addressHash, int type,
                       std::vector<CAddressUnspentDbEntry>& unspentOutputs)
{
    if (!fAddressIndex)
        return error("address index not enabled");

    if (!pblocktree->ReadAddressUnspentIndex(addressHash, type, unspentOutputs))
        return error("unable to get txids for address");

    return true;
}

/**
 * Return transaction in txOut, and if it was found inside a block, its hash is placed in hashBlock.
 * If blockIndex is provided, the transaction is fetched from the corresponding block.
 */
bool GetTransaction(const uint256& hash, CTransaction& txOut, const Consensus::Params& consensusParams, uint256& hashBlock, bool fAllowSlow, CBlockIndex* blockIndex)
{
    CBlockIndex* pindexSlow = blockIndex;

    LOCK(cs_main);

    if (!blockIndex) {
        if (mempool.lookup(hash, txOut))
        {
            return true;
        }

        if (fTxIndex) {
            CDiskTxPos postx;
            if (pblocktree->ReadTxIndex(hash, postx)) {
                CAutoFile file(OpenBlockFile(postx, true), SER_DISK, CLIENT_VERSION);
                if (file.IsNull())
                    return error("%s: OpenBlockFile failed", __func__);
                CBlockHeader header;
                try {
                    file >> header;
                    fseek(file.Get(), postx.nTxOffset, SEEK_CUR);
                    file >> txOut;
                } catch (const std::exception& e) {
                    return error("%s: Deserialize or I/O error - %s", __func__, e.what());
                }
                hashBlock = header.GetHash();
                if (txOut.GetHash() != hash)
                    return error("%s: txid mismatch", __func__);
                return true;
            }

            // transaction not found in index, nothing more can be done
            return false;
        }

        if (fAllowSlow) { // use coin database to locate block that contains transaction, and scan it
            int nHeight = -1;
            {
                CCoinsViewCache &view = *pcoinsTip;
                const CCoins* coins = view.AccessCoins(hash);
                if (coins)
                    nHeight = coins->nHeight;
            }
            if (nHeight > 0)
                pindexSlow = chainActive[nHeight];
        }
    }

    if (pindexSlow) {
        CBlock block;
        if (ReadBlockFromDisk(block, pindexSlow, consensusParams)) {
            for (const CTransaction &tx : block.vtx) {
                if (tx.GetHash() == hash) {
                    txOut = tx;
                    hashBlock = pindexSlow->GetBlockHash();
                    return true;
                }
            }
        }
    }

    return false;
}

//////////////////////////////////////////////////////////////////////////////
//
// CBlock and CBlockIndex
//

bool WriteBlockToDisk(const CBlock& block, CDiskBlockPos& pos, const CMessageHeader::MessageStartChars& messageStart)
{
    // Open history file to append
    CAutoFile fileout(OpenBlockFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("WriteBlockToDisk: OpenBlockFile failed");

    // Write index header
    unsigned int nSize = GetSerializeSize(fileout, block);
    fileout << FLATDATA(messageStart) << nSize;

    // Write block
    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0)
        return error("WriteBlockToDisk: ftell failed");
    pos.nPos = (unsigned int)fileOutPos;
    fileout << block;

    return true;
}

bool ReadBlockFromDisk(CBlock& block, const CDiskBlockPos& pos, const Consensus::Params& consensusParams)
{
    block.SetNull();

    // Open history file to read
    CAutoFile filein(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("ReadBlockFromDisk: OpenBlockFile failed for %s", pos.ToString());

    // Read block
    try {
        filein >> block;
    }
    catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s at %s", __func__, e.what(), pos.ToString());
    }

    // Check the header
    if (!(CheckEquihashSolution(&block, consensusParams) &&
          CheckProofOfWork(block.GetHash(), block.nBits, consensusParams)))
        return error("ReadBlockFromDisk: Errors in block header at %s", pos.ToString());

    return true;
}

bool ReadBlockFromDisk(CBlock& block, const CBlockIndex* pindex, const Consensus::Params& consensusParams)
{
    if (!ReadBlockFromDisk(block, pindex->GetBlockPos(), consensusParams))
        return false;
    if (block.GetHash() != pindex->GetBlockHash())
        return error("ReadBlockFromDisk(CBlock&, CBlockIndex*): GetHash() doesn't match index for %s at %s",
                pindex->ToString(), pindex->GetBlockPos().ToString());
    return true;
}

CAmount GetBlockSubsidy(int nHeight, const Consensus::Params& consensusParams)
{
    CAmount nSubsidy = 12.5 * COIN;

    // Mining slow start
    // The subsidy is ramped up linearly, skipping the middle payout of
    // MAX_SUBSIDY/2 to keep the monetary curve consistent with no slow start.
    if (nHeight < consensusParams.SubsidySlowStartShift()) {
        nSubsidy /= consensusParams.nSubsidySlowStartInterval;
        nSubsidy *= nHeight;
        return nSubsidy;
    } else if (nHeight < consensusParams.nSubsidySlowStartInterval) {
        nSubsidy /= consensusParams.nSubsidySlowStartInterval;
        nSubsidy *= (nHeight+1);
        return nSubsidy;
    }

    assert(nHeight >= consensusParams.SubsidySlowStartShift());

    int halvings = consensusParams.Halving(nHeight);

    // Force block reward to zero when right shift is undefined.
    if (halvings >= 64)
        return 0;

    // zip208
    // BlockSubsidy(height) :=
    // SlowStartRate · height, if height < SlowStartInterval / 2
    // SlowStartRate · (height + 1), if SlowStartInterval / 2 ≤ height and height < SlowStartInterval
    // floor(MaxBlockSubsidy / 2^Halving(height)), if SlowStartInterval ≤ height and not IsBlossomActivated(height)
    // floor(MaxBlockSubsidy / (BlossomPoWTargetSpacingRatio · 2^Halving(height))), otherwise
    if (consensusParams.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_BLOSSOM)) {
        return (nSubsidy / Consensus::BLOSSOM_POW_TARGET_SPACING_RATIO) >> halvings;
    } else {
        // Subsidy is cut in half every 840,000 blocks which will occur approximately every 4 years.
        return nSubsidy >> halvings;
    }
}

bool IsInitialBlockDownload(const Consensus::Params& params)
{
    // Once this function has returned false, it must remain false.
    static std::atomic<bool> latchToFalse{false};
    // Optimization: pre-test latch before taking the lock.
    if (latchToFalse.load(std::memory_order_relaxed))
        return false;

    LOCK(cs_main);
    if (latchToFalse.load(std::memory_order_relaxed))
        return false;
    if (fImporting || fReindex)
        return true;
    if (chainActive.Tip() == NULL)
        return true;
    if (chainActive.Tip()->nChainWork < UintToArith256(params.nMinimumChainWork))
        return true;
    // Don't bother checking Sprout, it is always active.
    for (int idx = Consensus::BASE_SPROUT + 1; idx < Consensus::MAX_NETWORK_UPGRADES; idx++) {
        // If we expect a particular activation block hash, and either the upgrade is not
        // active or it doesn't match the block at that height on the current chain, then
        // we are not on the correct chain. As we have already checked that the current
        // chain satisfies the minimum chain work, this is likely an adversarial situation
        // where the node is being fed a fake alternate chain; shut down for safety.
        //
        // Note that this depends on the assumption that if we set hashActivationBlock for
        // any upgrade, we also update nMinimumChainWork to be past that upgrade.
        //
        auto upgrade = params.vUpgrades[idx];
        if (upgrade.hashActivationBlock) {
            if (!params.NetworkUpgradeActive(chainActive.Height(), Consensus::UpgradeIndex(idx))) {
                AbortNode(
                    strprintf(
                        "%s: We are on a chain with sufficient work, but the %s network upgrade has not activated as expected.\n"
                        "Likely adversarial condition; shutting down for safety.\n"
                        "  nChainWork=%s\n  nMinimumChainWork=%s\n  tip height=%d\n  upgrade height=%d",
                        __func__,
                        NetworkUpgradeInfo[idx].strName,
                        chainActive.Tip()->nChainWork.GetHex(),
                        params.nMinimumChainWork.GetHex(),
                        chainActive.Height(),
                        upgrade.nActivationHeight),
                    _("We are on a chain with sufficient work, but an expected network upgrade has not activated. Your node may be under attack! Shutting down for safety."));
                return true;
            }

            // If an upgrade is active, we must be past its activation height.
            assert(chainActive[upgrade.nActivationHeight]);

            if (chainActive[upgrade.nActivationHeight]->GetBlockHash() != upgrade.hashActivationBlock.value()) {
                AbortNode(
                    strprintf(
                        "%s: We are on a chain with sufficient work, but the activation block hash for the %s network upgrade is not as expected.\n"
                        "Likely adversarial condition; shutting down for safety.\n",
                        "  nChainWork=%s\n  nMinimumChainWork=%s\n  tip height=%d\n  upgrade height=%d\n  expected hash=%s\n  actual hash=%s",
                        __func__,
                        NetworkUpgradeInfo[idx].strName,
                        chainActive.Tip()->nChainWork.GetHex(),
                        params.nMinimumChainWork.GetHex(),
                        chainActive.Height(),
                        upgrade.nActivationHeight,
                        upgrade.hashActivationBlock.value().GetHex(),
                        chainActive[upgrade.nActivationHeight]->GetBlockHash().GetHex()),
                    _("We are on a chain with sufficient work, but the network upgrade checkpoints do not match. Your node may be under attack! Shutting down for safety."));
                return true;
            }
        }
    }
    if (chainActive.Tip()->GetBlockTime() < (GetTime() - nMaxTipAge))
        return true;
    LogPrintf("Leaving InitialBlockDownload (latching to false)\n");
    latchToFalse.store(true, std::memory_order_relaxed);
    return false;
}

static CBlockIndex *pindexBestForkTip = NULL;
static CBlockIndex *pindexBestForkBase = NULL;

void CheckForkWarningConditions(const Consensus::Params& params)
{
    AssertLockHeld(cs_main);
    // Before we get past initial download, we cannot reliably alert about forks
    // (we assume we don't get stuck on a fork before finishing our initial sync)
    if (IsInitialBlockDownload(params))
        return;

    // If our best fork is no longer within 288 blocks (+/- 12 hours if no one mines it)
    // of our head, drop it
    if (pindexBestForkTip && chainActive.Height() - pindexBestForkTip->nHeight >= 288)
        pindexBestForkTip = NULL;

    if (pindexBestForkTip || (pindexBestInvalid && pindexBestInvalid->nChainWork > chainActive.Tip()->nChainWork + (GetBlockProof(*chainActive.Tip()) * 6)))
    {
        if (!GetfLargeWorkForkFound() && pindexBestForkBase)
        {
            std::string warning = std::string("'Warning: Large-work fork detected, forking after block ") +
                pindexBestForkBase->phashBlock->ToString() + std::string("'");
            CAlert::Notify(warning, true);
        }
        if (pindexBestForkTip && pindexBestForkBase)
        {
            LogPrintf("%s: Warning: Large valid fork found\n  forking the chain at height %d (%s)\n  lasting to height %d (%s).\nChain state database corruption likely.\n", __func__,
                   pindexBestForkBase->nHeight, pindexBestForkBase->phashBlock->ToString(),
                   pindexBestForkTip->nHeight, pindexBestForkTip->phashBlock->ToString());
            SetfLargeWorkForkFound(true);
        }
        else
        {
            std::string warning = std::string("Warning: Found invalid chain at least ~6 blocks longer than our best chain.\nChain state database corruption likely.");
            LogPrintf("%s: %s\n", warning.c_str(), __func__);
            CAlert::Notify(warning, true);
            SetfLargeWorkInvalidChainFound(true);
        }
    }
    else
    {
        SetfLargeWorkForkFound(false);
        SetfLargeWorkInvalidChainFound(false);
    }
}

void CheckForkWarningConditionsOnNewFork(CBlockIndex* pindexNewForkTip, const CChainParams& chainParams)
{
    AssertLockHeld(cs_main);
    // If we are on a fork that is sufficiently large, set a warning flag
    CBlockIndex* pfork = pindexNewForkTip;
    CBlockIndex* plonger = chainActive.Tip();
    while (pfork && pfork != plonger)
    {
        while (plonger && plonger->nHeight > pfork->nHeight)
            plonger = plonger->pprev;
        if (pfork == plonger)
            break;
        pfork = pfork->pprev;
    }

    // We define a condition where we should warn the user about as a fork of at least 7 blocks
    // with a tip within 72 blocks (+/- 3 hours if no one mines it) of ours
    // We use 7 blocks rather arbitrarily as it represents just under 10% of sustained network
    // hash rate operating on the fork.
    // or a chain that is entirely longer than ours and invalid (note that this should be detected by both)
    // We define it this way because it allows us to only store the highest fork tip (+ base) which meets
    // the 7-block condition and from this always have the most-likely-to-cause-warning fork
    if (pfork && (!pindexBestForkTip || (pindexBestForkTip && pindexNewForkTip->nHeight > pindexBestForkTip->nHeight)) &&
            pindexNewForkTip->nChainWork - pfork->nChainWork > (GetBlockProof(*pfork) * 7) &&
            chainActive.Height() - pindexNewForkTip->nHeight < 72)
    {
        pindexBestForkTip = pindexNewForkTip;
        pindexBestForkBase = pfork;
    }

    CheckForkWarningConditions(chainParams.GetConsensus());
}

// Requires cs_main.
void Misbehaving(NodeId pnode, int howmuch)
{
    if (howmuch == 0)
        return;

    CNodeState *state = State(pnode);
    if (state == NULL)
        return;

    state->nMisbehavior += howmuch;
    int banscore = GetArg("-banscore", DEFAULT_BANSCORE_THRESHOLD);
    if (state->nMisbehavior >= banscore && state->nMisbehavior - howmuch < banscore)
    {
        LogPrintf("%s: %s (%d -> %d) BAN THRESHOLD EXCEEDED\n", __func__, state->name, state->nMisbehavior-howmuch, state->nMisbehavior);
        state->fShouldBan = true;
    } else
        LogPrintf("%s: %s (%d -> %d)\n", __func__, state->name, state->nMisbehavior-howmuch, state->nMisbehavior);
}

void static InvalidChainFound(CBlockIndex* pindexNew, const CChainParams& chainParams)
{
    if (!pindexBestInvalid || pindexNew->nChainWork > pindexBestInvalid->nChainWork)
        pindexBestInvalid = pindexNew;

    LogPrintf("%s: invalid block=%s  height=%d  log2_work=%.8g  date=%s\n", __func__,
      pindexNew->GetBlockHash().ToString(), pindexNew->nHeight,
      log2(pindexNew->nChainWork.getdouble()), DateTimeStrFormat("%Y-%m-%d %H:%M:%S",
      pindexNew->GetBlockTime()));
    CBlockIndex *tip = chainActive.Tip();
    assert (tip);
    LogPrintf("%s:  current best=%s  height=%d  log2_work=%.8g  date=%s\n", __func__,
      tip->GetBlockHash().ToString(), chainActive.Height(), log2(tip->nChainWork.getdouble()),
      DateTimeStrFormat("%Y-%m-%d %H:%M:%S", tip->GetBlockTime()));
    CheckForkWarningConditions(chainParams.GetConsensus());
}

void static InvalidBlockFound(CBlockIndex *pindex, const CValidationState &state, const CChainParams& chainParams) {
    int nDoS = 0;
    if (state.IsInvalid(nDoS)) {
        std::map<uint256, NodeId>::iterator it = mapBlockSource.find(pindex->GetBlockHash());
        if (it != mapBlockSource.end() && State(it->second)) {
            CBlockReject reject = {state.GetRejectCode(), state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH), pindex->GetBlockHash()};
            State(it->second)->rejects.push_back(reject);
            if (nDoS > 0)
                Misbehaving(it->second, nDoS);
        }
    }
    if (!state.CorruptionPossible()) {
        pindex->nStatus |= BLOCK_FAILED_VALID;
        setDirtyBlockIndex.insert(pindex);
        setBlockIndexCandidates.erase(pindex);
        InvalidChainFound(pindex, chainParams);
    }
}

void UpdateCoins(const CTransaction& tx, CCoinsViewCache& inputs, CTxUndo &txundo, int nHeight)
{
    // mark inputs spent
    if (!tx.IsCoinBase()) {
        txundo.vprevout.reserve(tx.vin.size());
        for (const CTxIn &txin : tx.vin) {
            CCoinsModifier coins = inputs.ModifyCoins(txin.prevout.hash);
            unsigned nPos = txin.prevout.n;

            if (nPos >= coins->vout.size() || coins->vout[nPos].IsNull())
                assert(false);
            // mark an outpoint spent, and construct undo information
            txundo.vprevout.push_back(CTxInUndo(coins->vout[nPos]));
            coins->Spend(nPos);
            if (coins->vout.size() == 0) {
                CTxInUndo& undo = txundo.vprevout.back();
                undo.nHeight = coins->nHeight;
                undo.fCoinBase = coins->fCoinBase;
                undo.nVersion = coins->nVersion;
            }
        }
    }

    // spend nullifiers
    inputs.SetNullifiers(tx, true);

    // add outputs
    inputs.ModifyNewCoins(tx.GetHash())->FromTx(tx, nHeight);
}

void UpdateCoins(const CTransaction& tx, CCoinsViewCache& inputs, int nHeight)
{
    CTxUndo txundo;
    UpdateCoins(tx, inputs, txundo, nHeight);
}

bool CScriptCheck::operator()() {
    const CScript &scriptSig = ptxTo->vin[nIn].scriptSig;
    if (!VerifyScript(scriptSig, scriptPubKey, nFlags, CachingTransactionSignatureChecker(ptxTo, nIn, amount, cacheStore, *txdata), consensusBranchId, &error)) {
        return ::error("CScriptCheck(): %s:%d VerifySignature failed: %s", ptxTo->GetHash().ToString(), nIn, ScriptErrorString(error));
    }
    return true;
}

int GetSpendHeight(const CCoinsViewCache& inputs)
{
    LOCK(cs_main);
    CBlockIndex* pindexPrev = mapBlockIndex.find(inputs.GetBestBlock())->second;
    return pindexPrev->nHeight + 1;
}

namespace Consensus {
bool CheckTxInputs(const CTransaction& tx, CValidationState& state, const CCoinsViewCache& inputs, int nSpendHeight, const Consensus::Params& consensusParams)
{
        // This doesn't trigger the DoS code on purpose; if it did, it would make it easier
        // for an attacker to attempt to split the network.
        if (!inputs.HaveInputs(tx))
            return state.Invalid(error("CheckInputs(): %s inputs unavailable", tx.GetHash().ToString()));

        // Are the shielded spends' requirements met?
        auto unmetShieldedReq = inputs.HaveShieldedRequirements(tx);
        if (unmetShieldedReq) {
            auto txid = tx.GetHash().ToString();
            auto rejectCode = ShieldedReqRejectCode(*unmetShieldedReq);
            auto rejectReason = ShieldedReqRejectReason(*unmetShieldedReq);
            TracingError(
                "main", "CheckInputs(): shielded requirements not met",
                "txid", txid.c_str(),
                "reason", rejectReason.c_str());
            return state.Invalid(false, rejectCode, rejectReason);
        }

        CAmount nValueIn = 0;
        CAmount nFees = 0;
        for (unsigned int i = 0; i < tx.vin.size(); i++)
        {
            const COutPoint &prevout = tx.vin[i].prevout;
            const CCoins *coins = inputs.AccessCoins(prevout.hash);
            assert(coins);

            if (coins->IsCoinBase()) {
                // Ensure that coinbases are matured
                if (nSpendHeight - coins->nHeight < COINBASE_MATURITY) {
                    return state.Invalid(
                        error("CheckInputs(): tried to spend coinbase at depth %d", nSpendHeight - coins->nHeight),
                        REJECT_INVALID, "bad-txns-premature-spend-of-coinbase");
                }

                // Ensure that coinbases cannot be spent to transparent outputs
                // Disabled on regtest
                if (fCoinbaseEnforcedShieldingEnabled &&
                    consensusParams.fCoinbaseMustBeShielded &&
                    !tx.vout.empty()) {
                    return state.Invalid(
                        error("CheckInputs(): tried to spend coinbase with transparent outputs"),
                        REJECT_INVALID, "bad-txns-coinbase-spend-has-transparent-outputs");
                }
            }

            // Check for negative or overflow input values
            nValueIn += coins->vout[prevout.n].nValue;
            if (!MoneyRange(coins->vout[prevout.n].nValue) || !MoneyRange(nValueIn))
                return state.DoS(100, error("CheckInputs(): txin values out of range"),
                                 REJECT_INVALID, "bad-txns-inputvalues-outofrange");

        }

        nValueIn += tx.GetShieldedValueIn();
        if (!MoneyRange(nValueIn))
            return state.DoS(100, error("CheckInputs(): shielded input to transparent value pool out of range"),
                             REJECT_INVALID, "bad-txns-inputvalues-outofrange");

        if (nValueIn < tx.GetValueOut())
            return state.DoS(100, error("CheckInputs(): %s value in (%s) < value out (%s)",
                                        tx.GetHash().ToString(), FormatMoney(nValueIn), FormatMoney(tx.GetValueOut())),
                             REJECT_INVALID, "bad-txns-in-belowout");

        // Tally transaction fees
        CAmount nTxFee = nValueIn - tx.GetValueOut();
        if (nTxFee < 0)
            return state.DoS(100, error("CheckInputs(): %s nTxFee < 0", tx.GetHash().ToString()),
                             REJECT_INVALID, "bad-txns-fee-negative");
        nFees += nTxFee;
        if (!MoneyRange(nFees))
            return state.DoS(100, error("CheckInputs(): nFees out of range"),
                             REJECT_INVALID, "bad-txns-fee-outofrange");
    return true;
}
}// namespace Consensus

bool ContextualCheckInputs(
    const CTransaction& tx,
    CValidationState &state,
    const CCoinsViewCache &inputs,
    bool fScriptChecks,
    unsigned int flags,
    bool cacheStore,
    PrecomputedTransactionData& txdata,
    const Consensus::Params& consensusParams,
    uint32_t consensusBranchId,
    std::vector<CScriptCheck> *pvChecks)
{
    if (!tx.IsCoinBase())
    {
        if (!Consensus::CheckTxInputs(tx, state, inputs, GetSpendHeight(inputs), consensusParams)) {
            return false;
        }

        if (pvChecks)
            pvChecks->reserve(tx.vin.size());

        // The first loop above does all the inexpensive checks.
        // Only if ALL inputs pass do we perform expensive ECDSA signature checks.
        // Helps prevent CPU exhaustion attacks.

        // Skip ECDSA signature verification when connecting blocks
        // before the last block chain checkpoint. This is safe because block merkle hashes are
        // still computed and checked, and any change will be caught at the next checkpoint.
        if (fScriptChecks) {
            for (unsigned int i = 0; i < tx.vin.size(); i++) {
                const COutPoint &prevout = tx.vin[i].prevout;
                const CCoins* coins = inputs.AccessCoins(prevout.hash);
                assert(coins);

                // Verify signature
                CScriptCheck check(*coins, tx, i, flags, cacheStore, consensusBranchId, &txdata);
                if (pvChecks) {
                    pvChecks->push_back(CScriptCheck());
                    check.swap(pvChecks->back());
                } else if (!check()) {
                    // Check whether the failure was caused by an outdated
                    // consensus branch ID; if so, don't trigger DoS protection
                    // immediately, and inform the node that they need to
                    // upgrade. We only check the previous epoch's branch ID, on
                    // the assumption that users creating transactions will
                    // notice their transactions failing before a second network
                    // upgrade occurs.
                    auto prevConsensusBranchId = PrevEpochBranchId(consensusBranchId, consensusParams);
                    CScriptCheck checkPrev(*coins, tx, i, flags, cacheStore, prevConsensusBranchId, &txdata);
                    if (checkPrev()) {
                        return state.DoS(
                            10, false, REJECT_INVALID, strprintf(
                                "old-consensus-branch-id (Expected %s, found %s)",
                                HexInt(consensusBranchId),
                                HexInt(prevConsensusBranchId)));
                    }
                    if (flags & STANDARD_NOT_MANDATORY_VERIFY_FLAGS) {
                        // Check whether the failure was caused by a
                        // non-mandatory script verification check, such as
                        // non-standard DER encodings or non-null dummy
                        // arguments; if so, don't trigger DoS protection to
                        // avoid splitting the network between upgraded and
                        // non-upgraded nodes.
                        CScriptCheck check2(*coins, tx, i,
                                flags & ~STANDARD_NOT_MANDATORY_VERIFY_FLAGS, cacheStore, consensusBranchId, &txdata);
                        if (check2())
                            return state.Invalid(false, REJECT_NONSTANDARD, strprintf("non-mandatory-script-verify-flag (%s)", ScriptErrorString(check.GetScriptError())));
                    }
                    // Failures of other flags indicate a transaction that is
                    // invalid in new blocks, e.g. a invalid P2SH. We DoS ban
                    // such nodes as they are not following the protocol.
                    return state.DoS(100,false, REJECT_INVALID, strprintf("mandatory-script-verify-flag-failed (%s)", ScriptErrorString(check.GetScriptError())));
                }
            }
        }
    }

    return true;
}

namespace {

bool UndoWriteToDisk(const CBlockUndo& blockundo, CDiskBlockPos& pos, const uint256& hashBlock, const CMessageHeader::MessageStartChars& messageStart)
{
    // Open history file to append
    CAutoFile fileout(OpenUndoFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s: OpenUndoFile failed", __func__);

    // Write index header
    unsigned int nSize = GetSerializeSize(fileout, blockundo);
    fileout << FLATDATA(messageStart) << nSize;

    // Write undo data
    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0)
        return error("%s: ftell failed", __func__);
    pos.nPos = (unsigned int)fileOutPos;
    fileout << blockundo;

    // calculate & write checksum
    CHashWriter hasher(SER_GETHASH, PROTOCOL_VERSION);
    hasher << hashBlock;
    hasher << blockundo;
    fileout << hasher.GetHash();

    return true;
}

bool UndoReadFromDisk(CBlockUndo& blockundo, const CDiskBlockPos& pos, const uint256& hashBlock)
{
    // Open history file to read
    CAutoFile filein(OpenUndoFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("%s: OpenBlockFile failed", __func__);

    // Read block
    uint256 hashChecksum;
    try {
        filein >> blockundo;
        filein >> hashChecksum;
    }
    catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s", __func__, e.what());
    }

    // Verify checksum
    CHashWriter hasher(SER_GETHASH, PROTOCOL_VERSION);
    hasher << hashBlock;
    hasher << blockundo;
    if (hashChecksum != hasher.GetHash())
        return error("%s: Checksum mismatch", __func__);

    return true;
}

} // anon namespace

/**
 * Apply the undo operation of a CTxInUndo to the given chain state.
 * @param undo The undo object.
 * @param view The coins view to which to apply the changes.
 * @param out The out point that corresponds to the tx input.
 * @return True on success.
 */
static bool ApplyTxInUndo(const CTxInUndo& undo, CCoinsViewCache& view, const COutPoint& out)
{
    bool fClean = true;

    CCoinsModifier coins = view.ModifyCoins(out.hash);
    if (undo.nHeight != 0) {
        // undo data contains height: this is the last output of the prevout tx being spent
        if (!coins->IsPruned())
            fClean = fClean && error("%s: undo data overwriting existing transaction", __func__);
        coins->Clear();
        coins->fCoinBase = undo.fCoinBase;
        coins->nHeight = undo.nHeight;
        coins->nVersion = undo.nVersion;
    } else {
        if (coins->IsPruned())
            fClean = fClean && error("%s: undo data adding output to missing transaction", __func__);
    }
    if (coins->IsAvailable(out.n))
        fClean = fClean && error("%s: undo data overwriting existing output", __func__);
    if (coins->vout.size() < out.n+1)
        coins->vout.resize(out.n+1);
    coins->vout[out.n] = undo.txout;

    return fClean;
}

enum DisconnectResult
{
    DISCONNECT_OK,      // All good.
    DISCONNECT_UNCLEAN, // Rolled back, but UTXO set was inconsistent with block.
    DISCONNECT_FAILED   // Something else went wrong.
};

/** Undo the effects of this block (with given index) on the UTXO set represented by coins.
 *  When UNCLEAN or FAILED is returned, view is left in an indeterminate state.
 *  The addressIndex and spentIndex will be updated if requested.
 */
static DisconnectResult DisconnectBlock(const CBlock& block, CValidationState& state,
    const CBlockIndex* pindex, CCoinsViewCache& view, const CChainParams& chainparams,
    const bool updateIndices)
{
    assert(pindex->GetBlockHash() == view.GetBestBlock());

    bool fClean = true;

    CBlockUndo blockUndo;
    CDiskBlockPos pos = pindex->GetUndoPos();
    if (pos.IsNull()) {
        error("DisconnectBlock(): no undo data available");
        return DISCONNECT_FAILED;
    }
    if (!UndoReadFromDisk(blockUndo, pos, pindex->pprev->GetBlockHash())) {
        error("DisconnectBlock(): failure reading undo data");
        return DISCONNECT_FAILED;
    }

    if (blockUndo.vtxundo.size() + 1 != block.vtx.size()) {
        error("DisconnectBlock(): block and undo data inconsistent");
        return DISCONNECT_FAILED;
    }
    std::vector<CAddressIndexDbEntry> addressIndex;
    std::vector<CAddressUnspentDbEntry> addressUnspentIndex;
    std::vector<CSpentIndexDbEntry> spentIndex;

    // undo transactions in reverse order
    for (int i = block.vtx.size() - 1; i >= 0; i--) {
        const CTransaction &tx = block.vtx[i];
        uint256 const hash = tx.GetHash();

        // insightexplorer
        // https://github.com/bitpay/bitcoin/commit/017f548ea6d89423ef568117447e61dd5707ec42#diff-7ec3c68a81efff79b6ca22ac1f1eabbaR2236
        if (fAddressIndex && updateIndices) {
            for (unsigned int k = tx.vout.size(); k-- > 0;) {
                const CTxOut &out = tx.vout[k];
                CScript::ScriptType scriptType = out.scriptPubKey.GetType();
                if (scriptType != CScript::UNKNOWN) {
                    uint160 const addrHash = out.scriptPubKey.AddressHash();

                    // undo receiving activity
                    addressIndex.push_back(make_pair(
                        CAddressIndexKey(scriptType, addrHash, pindex->nHeight, i, hash, k, false),
                        out.nValue));

                    // undo unspent index
                    addressUnspentIndex.push_back(make_pair(
                        CAddressUnspentKey(scriptType, addrHash, hash, k),
                        CAddressUnspentValue()));
                }
            }
        }

        // Check that all outputs are available and match the outputs in the block itself
        // exactly.
        {
        CCoinsModifier outs = view.ModifyCoins(hash);
        outs->ClearUnspendable();

        CCoins outsBlock(tx, pindex->nHeight);
        // The CCoins serialization does not serialize negative numbers.
        // No network rules currently depend on the version here, so an inconsistency is harmless
        // but it must be corrected before txout nversion ever influences a network rule.
        if (outsBlock.nVersion < 0)
            outs->nVersion = outsBlock.nVersion;
        if (*outs != outsBlock)
            fClean = fClean && error("DisconnectBlock(): added transaction mismatch? database corrupted");

        // remove outputs
        outs->Clear();
        }

        // unspend nullifiers
        view.SetNullifiers(tx, false);

        // restore inputs
        if (i > 0) { // not coinbases
            const CTxUndo &txundo = blockUndo.vtxundo[i-1];
            if (txundo.vprevout.size() != tx.vin.size()) {
                error("DisconnectBlock(): transaction and undo data inconsistent");
                return DISCONNECT_FAILED;
            }
            for (unsigned int j = tx.vin.size(); j-- > 0;) {
                const COutPoint &out = tx.vin[j].prevout;
                const CTxInUndo &undo = txundo.vprevout[j];
                if (!ApplyTxInUndo(undo, view, out))
                    fClean = false;

                // insightexplorer
                // https://github.com/bitpay/bitcoin/commit/017f548ea6d89423ef568117447e61dd5707ec42#diff-7ec3c68a81efff79b6ca22ac1f1eabbaR2304
                const CTxIn input = tx.vin[j];
                if (fAddressIndex && updateIndices) {
                    const CTxOut &prevout = view.GetOutputFor(input);
                    CScript::ScriptType scriptType = prevout.scriptPubKey.GetType();
                    if (scriptType != CScript::UNKNOWN) {
                        uint160 const addrHash = prevout.scriptPubKey.AddressHash();

                        // undo spending activity
                        addressIndex.push_back(make_pair(
                            CAddressIndexKey(scriptType, addrHash, pindex->nHeight, i, hash, j, true),
                            prevout.nValue * -1));

                        // restore unspent index
                        addressUnspentIndex.push_back(make_pair(
                            CAddressUnspentKey(scriptType, addrHash, input.prevout.hash, input.prevout.n),
                            CAddressUnspentValue(prevout.nValue, prevout.scriptPubKey, undo.nHeight)));
                    }
                }
                // insightexplorer
                if (fSpentIndex && updateIndices) {
                    // undo and delete the spent index
                    spentIndex.push_back(make_pair(
                        CSpentIndexKey(input.prevout.hash, input.prevout.n),
                        CSpentIndexValue()));
                }
            }
        }
    }

    // set the old best Sprout anchor back
    view.PopAnchor(blockUndo.old_sprout_tree_root, SPROUT);

    // set the old best Sapling anchor back
    // We can get this from the `hashFinalSaplingRoot` of the last block
    // However, this is only reliable if the last block was on or after
    // the Sapling activation height. Otherwise, the last anchor was the
    // empty root.
    if (chainparams.GetConsensus().NetworkUpgradeActive(pindex->pprev->nHeight, Consensus::UPGRADE_SAPLING)) {
        view.PopAnchor(pindex->pprev->hashFinalSaplingRoot, SAPLING);
    } else {
        view.PopAnchor(SaplingMerkleTree::empty_root(), SAPLING);
    }

    // This is guaranteed to be filled by LoadBlockIndex.
    assert(pindex->nCachedBranchId);
    auto consensusBranchId = pindex->nCachedBranchId.value();

    if (chainparams.GetConsensus().NetworkUpgradeActive(pindex->nHeight, Consensus::UPGRADE_HEARTWOOD)) {
        view.PopHistoryNode(consensusBranchId);
    }

    // move best block pointer to prevout block
    view.SetBestBlock(pindex->pprev->GetBlockHash());

    // insightexplorer
    if (fAddressIndex && updateIndices) {
        if (!pblocktree->EraseAddressIndex(addressIndex)) {
            AbortNode(state, "Failed to delete address index");
            return DISCONNECT_FAILED;
        }
        if (!pblocktree->UpdateAddressUnspentIndex(addressUnspentIndex)) {
            AbortNode(state, "Failed to write address unspent index");
            return DISCONNECT_FAILED;
        }
    }
    // insightexplorer
    if (fSpentIndex && updateIndices) {
        if (!pblocktree->UpdateSpentIndex(spentIndex)) {
            AbortNode(state, "Failed to write transaction index");
            return DISCONNECT_FAILED;
        }
    }
    return fClean ? DISCONNECT_OK : DISCONNECT_UNCLEAN;
}

void static FlushBlockFile(bool fFinalize = false)
{
    LOCK(cs_LastBlockFile);

    CDiskBlockPos posOld(nLastBlockFile, 0);

    FILE *fileOld = OpenBlockFile(posOld);
    if (fileOld) {
        if (fFinalize)
            TruncateFile(fileOld, vinfoBlockFile[nLastBlockFile].nSize);
        FileCommit(fileOld);
        fclose(fileOld);
    }

    fileOld = OpenUndoFile(posOld);
    if (fileOld) {
        if (fFinalize)
            TruncateFile(fileOld, vinfoBlockFile[nLastBlockFile].nUndoSize);
        FileCommit(fileOld);
        fclose(fileOld);
    }
}

bool FindUndoPos(CValidationState &state, int nFile, CDiskBlockPos &pos, unsigned int nAddSize);

static CCheckQueue<CScriptCheck> scriptcheckqueue(128);

void ThreadScriptCheck() {
    RenameThread("zcash-scriptch");
    scriptcheckqueue.Thread();
}

static int64_t nTimeVerify = 0;
static int64_t nTimeConnect = 0;
static int64_t nTimeIndex = 0;
static int64_t nTimeCallbacks = 0;
static int64_t nTimeTotal = 0;

/**
 * Determine whether to do transaction checks when verifying blocks.
 * Returns `false` (allowing transaction checks to be skipped) only if all
 * of the following are true:
 *   - we're currently in initial block download
 *   - the `-ibdskiptxverification` flag is set
 *   - the block under inspection is an ancestor of the latest checkpoint.
 */
static bool ShouldCheckTransactions(const CChainParams& chainparams, const CBlockIndex* pindex) {
    return !(fIBDSkipTxVerification
             && fCheckpointsEnabled
             && IsInitialBlockDownload(chainparams.GetConsensus())
             && Checkpoints::IsAncestorOfLastCheckpoint(chainparams.Checkpoints(), pindex));
}

bool ConnectBlock(const CBlock& block, CValidationState& state, CBlockIndex* pindex,
                  CCoinsViewCache& view, const CChainParams& chainparams,
                  bool fJustCheck, bool fCheckAuthDataRoot)
{
    AssertLockHeld(cs_main);

    bool fExpensiveChecks = true;

    // If this block is an ancestor of a checkpoint, disable expensive checks
    if (fCheckpointsEnabled && Checkpoints::IsAncestorOfLastCheckpoint(chainparams.Checkpoints(), pindex)) {
        fExpensiveChecks = false;
    }

    // proof verification is expensive, disable if possible
    auto verifier = fExpensiveChecks ? ProofVerifier::Strict() : ProofVerifier::Disabled();

    // Disable Orchard batch signature validation if possible.
    auto orchardAuth = fExpensiveChecks ?
        orchard::AuthValidator::Batch() : orchard::AuthValidator::Disabled();

    // If in initial block download, and this block is an ancestor of a checkpoint,
    // and -ibdskiptxverification is set, disable all transaction checks.
    bool fCheckTransactions = ShouldCheckTransactions(chainparams, pindex);

    // Check it again to verify JoinSplit proofs, and in case a previous version let a bad block in
    if (!CheckBlock(block, state, chainparams, verifier, orchardAuth,
        !fJustCheck, !fJustCheck, fCheckTransactions))
    {
        return false;
    }

    // verify that the view's current state corresponds to the previous block
    uint256 hashPrevBlock = pindex->pprev == NULL ? uint256() : pindex->pprev->GetBlockHash();
    assert(hashPrevBlock == view.GetBestBlock());

    // Special case for the genesis block, skipping connection of its transactions
    // (its coinbase is unspendable)
    if (block.GetHash() == chainparams.GetConsensus().hashGenesisBlock) {
        if (!fJustCheck) {
            view.SetBestBlock(pindex->GetBlockHash());
            // Before the genesis block, there was an empty tree
            SproutMerkleTree tree;
            pindex->hashSproutAnchor = tree.root();
            // The genesis block contained no JoinSplits
            pindex->hashFinalSproutRoot = pindex->hashSproutAnchor;
        }
        return true;
    }

    // Reject a block that results in a negative shielded value pool balance.
    if (chainparams.ZIP209Enabled()) {
        // Sprout
        //
        // We can expect nChainSproutValue to be valid after the hardcoded
        // height, and this will be enforced on all descendant blocks. If
        // the node was reindexed then this will be enforced for all blocks.
        if (pindex->nChainSproutValue) {
            if (*pindex->nChainSproutValue < 0) {
                return state.DoS(100, error("ConnectBlock(): turnstile violation in Sprout shielded value pool"),
                             REJECT_INVALID, "turnstile-violation-sprout-shielded-pool");
            }
        }

        // Sapling
        //
        // If we've reached ConnectBlock, we have all transactions of
        // parents and can expect nChainSaplingValue not to be std::nullopt.
        // However, the miner and mining RPCs may not have populated this
        // value and will call `TestBlockValidity`. So, we act
        // conditionally.
        if (pindex->nChainSaplingValue) {
            if (*pindex->nChainSaplingValue < 0) {
                return state.DoS(100, error("ConnectBlock(): turnstile violation in Sapling shielded value pool"),
                             REJECT_INVALID, "turnstile-violation-sapling-shielded-pool");
            }
        }

        // Orchard
        //
        // If we've reached ConnectBlock, we have all transactions of
        // parents and can expect nChainOrchardValue not to be std::nullopt.
        // However, the miner and mining RPCs may not have populated this
        // value and will call `TestBlockValidity`. So, we act
        // conditionally.
        if (pindex->nChainOrchardValue) {
            if (*pindex->nChainOrchardValue < 0) {
                return state.DoS(100, error("ConnectBlock(): turnstile violation in Orchard shielded value pool"),
                                 REJECT_INVALID, "turnstile-violation-orchard");
            }
        }
    }

    // Do not allow blocks that contain transactions which 'overwrite' older transactions,
    // unless those are already completely spent.
    for (const CTransaction& tx : block.vtx) {
        const CCoins* coins = view.AccessCoins(tx.GetHash());
        if (coins && !coins->IsPruned())
            return state.DoS(100, error("ConnectBlock(): tried to overwrite transaction"),
                             REJECT_INVALID, "bad-txns-BIP30");
    }

    unsigned int flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;

    // DERSIG (BIP66) is also always enforced, but does not have a flag.

    CBlockUndo blockundo;

    CCheckQueueControl<CScriptCheck> control(fExpensiveChecks && nScriptCheckThreads ? &scriptcheckqueue : NULL);

    int64_t nTimeStart = GetTimeMicros();
    CAmount nFees = 0;
    int nInputs = 0;
    unsigned int nSigOps = 0;
    CDiskTxPos pos(pindex->GetBlockPos(), GetSizeOfCompactSize(block.vtx.size()));
    std::vector<std::pair<uint256, CDiskTxPos> > vPos;
    vPos.reserve(block.vtx.size());
    blockundo.vtxundo.reserve(block.vtx.size() - 1);
    std::vector<CAddressIndexDbEntry> addressIndex;
    std::vector<CAddressUnspentDbEntry> addressUnspentIndex;
    std::vector<CSpentIndexDbEntry> spentIndex;

    // Construct the incremental merkle tree at the current
    // block position,
    auto old_sprout_tree_root = view.GetBestAnchor(SPROUT);
    // saving the top anchor in the block index as we go.
    if (!fJustCheck) {
        pindex->hashSproutAnchor = old_sprout_tree_root;
    }
    SproutMerkleTree sprout_tree;
    // This should never fail: we should always be able to get the root
    // that is on the tip of our chain
    assert(view.GetSproutAnchorAt(old_sprout_tree_root, sprout_tree));
    {
        // Consistency check: the root of the tree we're given should
        // match what we asked for.
        assert(sprout_tree.root() == old_sprout_tree_root);
    }

    SaplingMerkleTree sapling_tree;
    assert(view.GetSaplingAnchorAt(view.GetBestAnchor(SAPLING), sapling_tree));

    OrchardMerkleTree orchard_tree;
    assert(view.GetOrchardAnchorAt(view.GetBestAnchor(ORCHARD), orchard_tree));

    // Grab the consensus branch ID for this block and its parent
    auto consensusBranchId = CurrentEpochBranchId(pindex->nHeight, chainparams.GetConsensus());
    auto prevConsensusBranchId = CurrentEpochBranchId(pindex->nHeight - 1, chainparams.GetConsensus());

    size_t total_sapling_tx = 0;
    size_t total_orchard_tx = 0;

    std::vector<PrecomputedTransactionData> txdata;
    txdata.reserve(block.vtx.size()); // Required so that pointers to individual PrecomputedTransactionData don't get invalidated
    for (unsigned int i = 0; i < block.vtx.size(); i++)
    {
        const CTransaction &tx = block.vtx[i];
        const uint256 hash = tx.GetHash();

        nInputs += tx.vin.size();
        nSigOps += GetLegacySigOpCount(tx);
        if (nSigOps > MAX_BLOCK_SIGOPS)
            return state.DoS(100, error("ConnectBlock(): too many sigops"),
                             REJECT_INVALID, "bad-blk-sigops");

        if (!tx.IsCoinBase())
        {
            if (!view.HaveInputs(tx))
                return state.DoS(100, error("ConnectBlock(): inputs missing/spent"),
                                 REJECT_INVALID, "bad-txns-inputs-missingorspent");

            // Are the shielded spends' requirements met?
            auto unmetShieldedReq = view.HaveShieldedRequirements(tx);
            if (unmetShieldedReq) {
                auto txid = tx.GetHash().ToString();
                auto rejectCode = ShieldedReqRejectCode(*unmetShieldedReq);
                auto rejectReason = ShieldedReqRejectReason(*unmetShieldedReq);
                TracingError(
                    "main", "ConnectBlock(): shielded requirements not met",
                    "txid", txid.c_str(),
                    "reason", rejectReason.c_str());
                return state.DoS(100, false, rejectCode, rejectReason);
            }

            // insightexplorer
            // https://github.com/bitpay/bitcoin/commit/017f548ea6d89423ef568117447e61dd5707ec42#diff-7ec3c68a81efff79b6ca22ac1f1eabbaR2597
            if (fAddressIndex || fSpentIndex) {
                for (size_t j = 0; j < tx.vin.size(); j++) {

                    const CTxIn input = tx.vin[j];
                    const CTxOut &prevout = view.GetOutputFor(tx.vin[j]);
                    CScript::ScriptType scriptType = prevout.scriptPubKey.GetType();
                    const uint160 addrHash = prevout.scriptPubKey.AddressHash();
                    if (fAddressIndex && scriptType != CScript::UNKNOWN) {
                        // record spending activity
                        addressIndex.push_back(make_pair(
                            CAddressIndexKey(scriptType, addrHash, pindex->nHeight, i, hash, j, true),
                            prevout.nValue * -1));

                        // remove address from unspent index
                        addressUnspentIndex.push_back(make_pair(
                            CAddressUnspentKey(scriptType, addrHash, input.prevout.hash, input.prevout.n),
                            CAddressUnspentValue()));
                    }
                    if (fSpentIndex) {
                        // Add the spent index to determine the txid and input that spent an output
                        // and to find the amount and address from an input.
                        // If we do not recognize the script type, we still add an entry to the
                        // spentindex db, with a script type of 0 and addrhash of all zeroes.
                        spentIndex.push_back(make_pair(
                            CSpentIndexKey(input.prevout.hash, input.prevout.n),
                            CSpentIndexValue(hash, j, pindex->nHeight, prevout.nValue, scriptType, addrHash)));
                    }
                }
            }

            // Add in sigops done by pay-to-script-hash inputs;
            // this is to prevent a "rogue miner" from creating
            // an incredibly-expensive-to-validate block.
            nSigOps += GetP2SHSigOpCount(tx, view);
            if (nSigOps > MAX_BLOCK_SIGOPS)
                return state.DoS(100, error("ConnectBlock(): too many sigops"),
                                 REJECT_INVALID, "bad-blk-sigops");
        }

        txdata.emplace_back(tx);

        if (!tx.IsCoinBase())
        {
            nFees += view.GetValueIn(tx)-tx.GetValueOut();

            std::vector<CScriptCheck> vChecks;
            bool fCacheResults = fJustCheck; /* Don't cache results if we're actually connecting blocks (still consult the cache, though) */
            if (!ContextualCheckInputs(tx, state, view, fExpensiveChecks, flags, fCacheResults, txdata[i], chainparams.GetConsensus(), consensusBranchId, nScriptCheckThreads ? &vChecks : NULL))
                return false;
            control.Add(vChecks);
        }

        // insightexplorer
        // https://github.com/bitpay/bitcoin/commit/017f548ea6d89423ef568117447e61dd5707ec42#diff-7ec3c68a81efff79b6ca22ac1f1eabbaR2656
        if (fAddressIndex) {
            for (unsigned int k = 0; k < tx.vout.size(); k++) {
                const CTxOut &out = tx.vout[k];
                CScript::ScriptType scriptType = out.scriptPubKey.GetType();
                if (scriptType != CScript::UNKNOWN) {
                    uint160 const addrHash = out.scriptPubKey.AddressHash();

                    // record receiving activity
                    addressIndex.push_back(make_pair(
                        CAddressIndexKey(scriptType, addrHash, pindex->nHeight, i, hash, k, false),
                        out.nValue));

                    // record unspent output
                    addressUnspentIndex.push_back(make_pair(
                        CAddressUnspentKey(scriptType, addrHash, hash, k),
                        CAddressUnspentValue(out.nValue, out.scriptPubKey, pindex->nHeight)));
                }
            }
        }

        CTxUndo undoDummy;
        if (i > 0) {
            blockundo.vtxundo.push_back(CTxUndo());
        }
        UpdateCoins(tx, view, i == 0 ? undoDummy : blockundo.vtxundo.back(), pindex->nHeight);

        for (const JSDescription &joinsplit : tx.vJoinSplit) {
            for (const uint256 &note_commitment : joinsplit.commitments) {
                // Insert the note commitments into our temporary tree.

                sprout_tree.append(note_commitment);
            }
        }

        for (const OutputDescription &outputDescription : tx.vShieldedOutput) {
            sapling_tree.append(outputDescription.cmu);
        }

        if (!orchard_tree.AppendBundle(tx.GetOrchardBundle())) {
            return state.DoS(100,
                error("ConnectBlock(): block would overfill the Orchard commitment tree."),
                REJECT_INVALID, "orchard-commitment-tree-full");
        };

        if (!(tx.vShieldedSpend.empty() && tx.vShieldedOutput.empty())) {
            total_sapling_tx += 1;
        }

        if (tx.GetOrchardBundle().IsPresent()) {
            total_orchard_tx += 1;
        }

        vPos.push_back(std::make_pair(tx.GetHash(), pos));
        pos.nTxOffset += ::GetSerializeSize(tx, SER_DISK, CLIENT_VERSION);
    }

    // Derive the various block commitments.
    // We only derive them if they will be used for this block.
    std::optional<uint256> hashAuthDataRoot;
    std::optional<uint256> hashChainHistoryRoot;
    if (chainparams.GetConsensus().NetworkUpgradeActive(pindex->nHeight, Consensus::UPGRADE_NU5)) {
        hashAuthDataRoot = block.BuildAuthDataMerkleTree();
    }
    if (chainparams.GetConsensus().NetworkUpgradeActive(pindex->nHeight, Consensus::UPGRADE_HEARTWOOD)) {
        hashChainHistoryRoot = view.GetHistoryRoot(prevConsensusBranchId);
    }

    view.PushAnchor(sprout_tree);
    view.PushAnchor(sapling_tree);
    view.PushAnchor(orchard_tree);
    if (!fJustCheck) {
        pindex->hashFinalSproutRoot = sprout_tree.root();
        // - If this block is before Heartwood activation, then we don't set
        //   hashFinalSaplingRoot here to maintain the invariant documented in
        //   CBlockIndex (which was ensured in AddToBlockIndex).
        // - If this block is on or after Heartwood activation, this is where we
        //   set the correct value of hashFinalSaplingRoot; in particular,
        //   blocks that are never passed to ConnectBlock() (and thus never on
        //   the main chain) will stay with hashFinalSaplingRoot set to null.
        if (chainparams.GetConsensus().NetworkUpgradeActive(pindex->nHeight, Consensus::UPGRADE_HEARTWOOD)) {
            pindex->hashFinalSaplingRoot = sapling_tree.root();
        }

        // - If this block is before NU5 activation:
        //   - hashAuthDataRoot and hashFinalOrchardRoot are always null.
        //   - We don't set hashChainHistoryRoot here to maintain the invariant
        //     documented in CBlockIndex (which was ensured in AddToBlockIndex).
        // - If this block is on or after NU5 activation, this is where we set
        //   the correct values of hashAuthDataRoot, hashFinalOrchardRoot, and
        //   hashChainHistoryRoot; in particular, blocks that are never passed
        //   to ConnectBlock() (and thus never on the main chain) will stay with
        //   these set to null.
        if (chainparams.GetConsensus().NetworkUpgradeActive(pindex->nHeight, Consensus::UPGRADE_NU5)) {
            pindex->hashAuthDataRoot = hashAuthDataRoot.value();
            pindex->hashFinalOrchardRoot = orchard_tree.root(),
            pindex->hashChainHistoryRoot = hashChainHistoryRoot.value();
        }
    }
    blockundo.old_sprout_tree_root = old_sprout_tree_root;

    if (chainparams.GetConsensus().NetworkUpgradeActive(pindex->nHeight, Consensus::UPGRADE_NU5)) {
        if (fCheckAuthDataRoot) {
            // If NU5 is active, block.hashBlockCommitments must be the top digest
            // of the ZIP 244 block commitments linked list.
            // https://zips.z.cash/zip-0244#block-header-changes
            uint256 hashBlockCommitments = DeriveBlockCommitmentsHash(
                hashChainHistoryRoot.value(),
                hashAuthDataRoot.value());
            if (block.hashBlockCommitments != hashBlockCommitments) {
                return state.DoS(100,
                    error("ConnectBlock(): block's hashBlockCommitments is incorrect (should be ZIP 244 block commitment)"),
                    REJECT_INVALID, "bad-block-commitments-hash");
            }
        }
    } else if (IsActivationHeight(pindex->nHeight, chainparams.GetConsensus(), Consensus::UPGRADE_HEARTWOOD)) {
        // In the block that activates ZIP 221, block.hashBlockCommitments MUST
        // be set to all zero bytes.
        if (!block.hashBlockCommitments.IsNull()) {
            return state.DoS(100,
                error("ConnectBlock(): block's hashBlockCommitments is incorrect (should be null)"),
                REJECT_INVALID, "bad-heartwood-root-in-block");
        }
    } else if (chainparams.GetConsensus().NetworkUpgradeActive(pindex->nHeight, Consensus::UPGRADE_HEARTWOOD)) {
        // If Heartwood is active, block.hashBlockCommitments must be the same as
        // the root of the history tree for the previous block. We only store
        // one tree per epoch, so we have two possible cases:
        // - If the previous block is in the previous epoch, this block won't
        //   affect that epoch's tree root.
        // - If the previous block is in this epoch, this block would affect
        //   this epoch's tree root, but as we haven't updated the tree for this
        //   block yet, view.GetHistoryRoot() returns the root we need.
        if (block.hashBlockCommitments != hashChainHistoryRoot.value()) {
            return state.DoS(100,
                error("ConnectBlock(): block's hashBlockCommitments is incorrect (should be history tree root)"),
                REJECT_INVALID, "bad-heartwood-root-in-block");
        }
    } else if (chainparams.GetConsensus().NetworkUpgradeActive(pindex->nHeight, Consensus::UPGRADE_SAPLING)) {
        // If Sapling is active, block.hashBlockCommitments must be the
        // same as the root of the Sapling tree
        if (block.hashBlockCommitments != sapling_tree.root()) {
            return state.DoS(100,
                error("ConnectBlock(): block's hashBlockCommitments is incorrect (should be Sapling tree root)"),
                REJECT_INVALID, "bad-sapling-root-in-block");
        }
    }

    // History read/write is started with Heartwood update.
    if (chainparams.GetConsensus().NetworkUpgradeActive(pindex->nHeight, Consensus::UPGRADE_HEARTWOOD)) {
        HistoryNode historyNode;
        if (chainparams.GetConsensus().NetworkUpgradeActive(pindex->nHeight, Consensus::UPGRADE_NU5)) {
            historyNode = libzcash::NewV2Leaf(
                block.GetHash(),
                block.nTime,
                block.nBits,
                pindex->hashFinalSaplingRoot,
                pindex->hashFinalOrchardRoot,
                ArithToUint256(GetBlockProof(*pindex)),
                pindex->nHeight,
                total_sapling_tx,
                total_orchard_tx
            );
        } else {
            historyNode = libzcash::NewV1Leaf(
                block.GetHash(),
                block.nTime,
                block.nBits,
                pindex->hashFinalSaplingRoot,
                ArithToUint256(GetBlockProof(*pindex)),
                pindex->nHeight,
                total_sapling_tx
            );
        }

        view.PushHistoryNode(consensusBranchId, historyNode);
    }

    int64_t nTime1 = GetTimeMicros(); nTimeConnect += nTime1 - nTimeStart;
    LogPrint("bench", "      - Connect %u transactions: %.2fms (%.3fms/tx, %.3fms/txin) [%.2fs]\n", (unsigned)block.vtx.size(), 0.001 * (nTime1 - nTimeStart), 0.001 * (nTime1 - nTimeStart) / block.vtx.size(), nInputs <= 1 ? 0 : 0.001 * (nTime1 - nTimeStart) / (nInputs-1), nTimeConnect * 0.000001);

    CAmount blockReward = nFees + GetBlockSubsidy(pindex->nHeight, chainparams.GetConsensus());
    if (block.vtx[0].GetValueOut() > blockReward)
        return state.DoS(100,
                         error("ConnectBlock(): coinbase pays too much (actual=%d vs limit=%d)",
                               block.vtx[0].GetValueOut(), blockReward),
                               REJECT_INVALID, "bad-cb-amount");

    // Ensure Orchard signatures are valid (if we are checking them)
    if (!orchardAuth.Validate()) {
        return state.DoS(100,
            error("ConnectBlock(): an Orchard bundle within the block is invalid"),
            REJECT_INVALID, "bad-orchard-bundle-authorization");
    }

    if (!control.Wait())
        return state.DoS(100, false);
    int64_t nTime2 = GetTimeMicros(); nTimeVerify += nTime2 - nTimeStart;
    LogPrint("bench", "    - Verify %u txins: %.2fms (%.3fms/txin) [%.2fs]\n", nInputs - 1, 0.001 * (nTime2 - nTimeStart), nInputs <= 1 ? 0 : 0.001 * (nTime2 - nTimeStart) / (nInputs-1), nTimeVerify * 0.000001);

    if (fJustCheck)
        return true;

    // Write undo information to disk
    if (pindex->GetUndoPos().IsNull() || !pindex->IsValid(BLOCK_VALID_SCRIPTS))
    {
        if (pindex->GetUndoPos().IsNull()) {
            CDiskBlockPos _pos;
            if (!FindUndoPos(state, pindex->nFile, _pos, ::GetSerializeSize(blockundo, SER_DISK, CLIENT_VERSION) + 40))
                return error("ConnectBlock(): FindUndoPos failed");
            if (!UndoWriteToDisk(blockundo, _pos, pindex->pprev->GetBlockHash(), chainparams.MessageStart()))
                return AbortNode(state, "Failed to write undo data");

            // update nUndoPos in block index
            pindex->nUndoPos = _pos.nPos;
            pindex->nStatus |= BLOCK_HAVE_UNDO;
        }

        // Now that all consensus rules have been validated, set nCachedBranchId.
        // Move this if BLOCK_VALID_CONSENSUS is ever altered.
        static_assert(BLOCK_VALID_CONSENSUS == BLOCK_VALID_SCRIPTS,
            "nCachedBranchId must be set after all consensus rules have been validated.");
        if (IsActivationHeightForAnyUpgrade(pindex->nHeight, chainparams.GetConsensus())) {
            pindex->nStatus |= BLOCK_ACTIVATES_UPGRADE;
            pindex->nCachedBranchId = CurrentEpochBranchId(pindex->nHeight, chainparams.GetConsensus());
        } else if (pindex->pprev) {
            pindex->nCachedBranchId = pindex->pprev->nCachedBranchId;
        }

        pindex->RaiseValidity(BLOCK_VALID_SCRIPTS);
        setDirtyBlockIndex.insert(pindex);
    }

    if (fTxIndex)
        if (!pblocktree->WriteTxIndex(vPos))
            return AbortNode(state, "Failed to write transaction index");

    // START insightexplorer
    if (fAddressIndex) {
        if (!pblocktree->WriteAddressIndex(addressIndex)) {
            return AbortNode(state, "Failed to write address index");
        }
        if (!pblocktree->UpdateAddressUnspentIndex(addressUnspentIndex)) {
            return AbortNode(state, "Failed to write address unspent index");
        }
    }
    if (fSpentIndex) {
        if (!pblocktree->UpdateSpentIndex(spentIndex)) {
            return AbortNode(state, "Failed to write spent index");
        }
    }
    if (fTimestampIndex) {
        unsigned int logicalTS = pindex->nTime;
        unsigned int prevLogicalTS = 0;

        // retrieve logical timestamp of the previous block
        if (pindex->pprev)
            if (!pblocktree->ReadTimestampBlockIndex(pindex->pprev->GetBlockHash(), prevLogicalTS))
                LogPrintf("%s: Failed to read previous block's logical timestamp\n", __func__);

        if (logicalTS <= prevLogicalTS) {
            logicalTS = prevLogicalTS + 1;
            LogPrintf("%s: Previous logical timestamp is newer Actual[%d] prevLogical[%d] Logical[%d]\n", __func__, pindex->nTime, prevLogicalTS, logicalTS);
        }

        if (!pblocktree->WriteTimestampIndex(CTimestampIndexKey(logicalTS, pindex->GetBlockHash())))
            return AbortNode(state, "Failed to write timestamp index");

        if (!pblocktree->WriteTimestampBlockIndex(CTimestampBlockIndexKey(pindex->GetBlockHash()), CTimestampBlockIndexValue(logicalTS)))
            return AbortNode(state, "Failed to write blockhash index");
    }
    // END insightexplorer

    // add this block to the view's block chain
    view.SetBestBlock(pindex->GetBlockHash());

    int64_t nTime3 = GetTimeMicros(); nTimeIndex += nTime3 - nTime2;
    LogPrint("bench", "    - Index writing: %.2fms [%.2fs]\n", 0.001 * (nTime3 - nTime2), nTimeIndex * 0.000001);

    // Watch for changes to the previous coinbase transaction.
    static uint256 hashPrevBestCoinBase;
    GetMainSignals().UpdatedTransaction(hashPrevBestCoinBase);
    hashPrevBestCoinBase = block.vtx[0].GetHash();

    int64_t nTime4 = GetTimeMicros(); nTimeCallbacks += nTime4 - nTime3;
    LogPrint("bench", "    - Callbacks: %.2fms [%.2fs]\n", 0.001 * (nTime4 - nTime3), nTimeCallbacks * 0.000001);

    return true;
}

enum FlushStateMode {
    FLUSH_STATE_NONE,
    FLUSH_STATE_IF_NEEDED,
    FLUSH_STATE_PERIODIC,
    FLUSH_STATE_ALWAYS
};

/**
 * Update the on-disk chain state.
 * The caches and indexes are flushed depending on the mode we're called with
 * if they're too large, if it's been a while since the last write,
 * or always and in all cases if we're in prune mode and are deleting files.
 */
bool static FlushStateToDisk(
    const CChainParams& chainparams,
    CValidationState &state,
    FlushStateMode mode) {
    LOCK2(cs_main, cs_LastBlockFile);
    static int64_t nLastWrite = 0;
    static int64_t nLastFlush = 0;
    std::set<int> setFilesToPrune;
    bool fFlushForPrune = false;
    try {
    if (fPruneMode && fCheckForPruning && !fReindex) {
        FindFilesToPrune(setFilesToPrune, chainparams.PruneAfterHeight());
        fCheckForPruning = false;
        if (!setFilesToPrune.empty()) {
            fFlushForPrune = true;
            if (!fHavePruned) {
                pblocktree->WriteFlag("prunedblockfiles", true);
                fHavePruned = true;
            }
        }
    }
    int64_t nNow = GetTimeMicros();
    // Avoid writing/flushing immediately after startup.
    if (nLastWrite == 0) {
        nLastWrite = nNow;
    }
    if (nLastFlush == 0) {
        nLastFlush = nNow;
    }
    size_t cacheSize = pcoinsTip->DynamicMemoryUsage();
    // The cache is large and close to the limit, but we have time now (not in the middle of a block processing).
    bool fCacheLarge = mode == FLUSH_STATE_PERIODIC && cacheSize * (10.0/9) > nCoinCacheUsage;
    // The cache is over the limit, we have to write now.
    bool fCacheCritical = mode == FLUSH_STATE_IF_NEEDED && cacheSize > nCoinCacheUsage;
    // It's been a while since we wrote the block index to disk. Do this frequently, so we don't need to redownload after a crash.
    bool fPeriodicWrite = mode == FLUSH_STATE_PERIODIC && nNow > nLastWrite + (int64_t)DATABASE_WRITE_INTERVAL * 1000000;
    // It's been very long since we flushed the cache. Do this infrequently, to optimize cache usage.
    bool fPeriodicFlush = mode == FLUSH_STATE_PERIODIC && nNow > nLastFlush + (int64_t)DATABASE_FLUSH_INTERVAL * 1000000;
    // Combine all conditions that result in a full cache flush.
    bool fDoFullFlush = (mode == FLUSH_STATE_ALWAYS) || fCacheLarge || fCacheCritical || fPeriodicFlush || fFlushForPrune;
    // Write blocks and block index to disk.
    if (fDoFullFlush || fPeriodicWrite) {
        // Depend on nMinDiskSpace to ensure we can write block index
        if (!CheckDiskSpace(0))
            return state.Error("out of disk space");
        // First make sure all block and undo data is flushed to disk.
        FlushBlockFile();
        // Then update all block file information (which may refer to block and undo files).
        {
            std::vector<std::pair<int, const CBlockFileInfo*> > vFiles;
            vFiles.reserve(setDirtyFileInfo.size());
            for (set<int>::iterator it = setDirtyFileInfo.begin(); it != setDirtyFileInfo.end(); ) {
                vFiles.push_back(make_pair(*it, &vinfoBlockFile[*it]));
                it = setDirtyFileInfo.erase(it);
            }
            std::vector<const CBlockIndex*> vBlocks;
            vBlocks.reserve(setDirtyBlockIndex.size());
            for (set<CBlockIndex*>::iterator it = setDirtyBlockIndex.begin(); it != setDirtyBlockIndex.end(); ) {
                vBlocks.push_back(*it);
                it = setDirtyBlockIndex.erase(it);
            }
            if (!pblocktree->WriteBatchSync(vFiles, nLastBlockFile, vBlocks)) {
                return AbortNode(state, "Files to write to block index database");
            }
        }
        // Finally remove any pruned files
        if (fFlushForPrune)
            UnlinkPrunedFiles(setFilesToPrune);
        nLastWrite = nNow;
    }
    // Flush best chain related state. This can only be done if the blocks / block index write was also done.
    if (fDoFullFlush) {
        // Typical CCoins structures on disk are around 128 bytes in size.
        // Pushing a new one to the database can cause it to be written
        // twice (once in the log, and once in the tables). This is already
        // an overestimation, as most will delete an existing entry or
        // overwrite one. Still, use a conservative safety factor of 2.
        if (!CheckDiskSpace(128 * 2 * 2 * pcoinsTip->GetCacheSize()))
            return state.Error("out of disk space");
        // Flush the chainstate (which may refer to block index entries).
        if (!pcoinsTip->Flush())
            return AbortNode(state, "Failed to write to coin database");
        nLastFlush = nNow;
    }
    // Don't flush the wallet witness cache (SetBestChain()) here, see #4301
    } catch (const std::runtime_error& e) {
        return AbortNode(state, std::string("System error while flushing: ") + e.what());
    }
    return true;
}

void FlushStateToDisk() {
    CValidationState state;
    FlushStateToDisk(Params(), state, FLUSH_STATE_ALWAYS);
}

void PruneAndFlush() {
    CValidationState state;
    fCheckForPruning = true;
    FlushStateToDisk(Params(), state, FLUSH_STATE_NONE);
}

struct PoolMetrics {
    std::optional<size_t> created;
    std::optional<size_t> spent;
    std::optional<size_t> unspent;
    std::optional<CAmount> value;

    static PoolMetrics Sprout(CBlockIndex *pindex, CCoinsViewCache *view) {
        PoolMetrics stats;
        stats.value = pindex->nChainSproutValue;

        // RewindBlockIndex calls DisconnectTip in a way that can potentially cause a
        // Sprout tree to not exist (the rewind_index RPC test reliably triggers this).
        // We only need to access the tree during disconnection for metrics purposes, and
        // we will never encounter this rewind situation on either mainnet or testnet, so
        // if we can't access the Sprout tree we default to zero.
        SproutMerkleTree sproutTree;
        if (view->GetSproutAnchorAt(pindex->hashFinalSproutRoot, sproutTree)) {
            stats.created = sproutTree.size();
        } else {
            stats.created = 0;
        }

        return stats;
    }

    static PoolMetrics Sapling(CBlockIndex *pindex, CCoinsViewCache *view) {
        PoolMetrics stats;
        stats.value = pindex->nChainSaplingValue;

        // Before Sapling activation, the Sapling commitment set is empty.
        SaplingMerkleTree saplingTree;
        if (view->GetSaplingAnchorAt(pindex->hashFinalSaplingRoot, saplingTree)) {
            stats.created = saplingTree.size();
        } else {
            stats.created = 0;
        }

        return stats;
    }

    static PoolMetrics Orchard(CBlockIndex *pindex, CCoinsViewCache *view) {
        PoolMetrics stats;
        stats.value = pindex->nChainOrchardValue;

        return stats;
    }

    static PoolMetrics Transparent(CBlockIndex *pindex, CCoinsViewCache *view) {
        PoolMetrics stats;
        // TODO: Collect transparent pool value.

        // TODO: Figure out a way to efficiently collect UTXO set metrics
        // (view->GetStats() is too slow to call during block verification).

        return stats;
    }
};

#define RenderPoolMetrics(poolName, poolMetrics) \
    do {                                         \
        if (poolMetrics.created) {               \
            MetricsStaticGauge(                  \
                "zcash.pool.notes.created",      \
                poolMetrics.created.value(),     \
                "name", poolName);               \
        }                                        \
        if (poolMetrics.spent) {                 \
            MetricsStaticGauge(                  \
                "zcash.pool.notes.spent",        \
                poolMetrics.spent.value(),       \
                "name", poolName);               \
        }                                        \
        if (poolMetrics.unspent) {               \
            MetricsStaticGauge(                  \
                "zcash.pool.notes.unspent",      \
                poolMetrics.unspent.value(),     \
                "name", poolName);               \
        }                                        \
        if (poolMetrics.value) {                 \
            MetricsStaticGauge(                  \
                "zcash.pool.value.zatoshis",     \
                poolMetrics.value.value(),       \
                "name", poolName);               \
        }                                        \
    } while (0)

/** Update chainActive and related internal data structures. */
void static UpdateTip(CBlockIndex *pindexNew, const CChainParams& chainParams) {
    chainActive.SetTip(pindexNew);

    // New best block
    nTimeBestReceived = GetTime();
    mempool.AddTransactionsUpdated(1);

    auto hash = tfm::format("%s", chainActive.Tip()->GetBlockHash().ToString());
    auto height = tfm::format("%d", chainActive.Height());
    auto bits = tfm::format("%d", chainActive.Tip()->nBits);
    auto log2_work = tfm::format("%.8g", log2(chainActive.Tip()->nChainWork.getdouble()));
    auto tx = tfm::format("%lu", (unsigned long)chainActive.Tip()->nChainTx);
    auto date = DateTimeStrFormat("%Y-%m-%d %H:%M:%S", chainActive.Tip()->GetBlockTime());
    auto progress = tfm::format("%f", Checkpoints::GuessVerificationProgress(chainParams.Checkpoints(), chainActive.Tip()));
    auto cache = tfm::format("%.1fMiB(%utx)", pcoinsTip->DynamicMemoryUsage() * (1.0 / (1<<20)), pcoinsTip->GetCacheSize());

    TracingInfo("main", "UpdateTip: new best",
        "hash", hash.c_str(),
        "height", height.c_str(),
        "bits", bits.c_str(),
        "log2_work", log2_work.c_str(),
        "tx", tx.c_str(),
        "date", date.c_str(),
        "progress", progress.c_str(),
        "cache", cache.c_str());

    auto sproutPool = PoolMetrics::Sprout(pindexNew, pcoinsTip);
    auto saplingPool = PoolMetrics::Sapling(pindexNew, pcoinsTip);
    auto transparentPool = PoolMetrics::Transparent(pindexNew, pcoinsTip);

    MetricsGauge("zcash.chain.verified.block.height", pindexNew->nHeight);
    RenderPoolMetrics("sprout", sproutPool);
    RenderPoolMetrics("sapling", saplingPool);
    RenderPoolMetrics("transparent", transparentPool);

    cvBlockChange.notify_all();
}

/**
 * Disconnect chainActive's tip. You probably want to call mempool.removeForReorg and
 * mempool.removeWithoutBranchId after this, with cs_main held.
 */
bool static DisconnectTip(CValidationState &state, const CChainParams& chainparams, bool fBare = false)
{
    CBlockIndex *pindexDelete = chainActive.Tip();
    assert(pindexDelete);
    // Read block from disk.
    CBlock block;
    if (!ReadBlockFromDisk(block, pindexDelete, chainparams.GetConsensus()))
        return AbortNode(state, "Failed to read block");
    // Apply the block atomically to the chain state.
    uint256 sproutAnchorBeforeDisconnect = pcoinsTip->GetBestAnchor(SPROUT);
    uint256 saplingAnchorBeforeDisconnect = pcoinsTip->GetBestAnchor(SAPLING);
    int64_t nStart = GetTimeMicros();
    {
        CCoinsViewCache view(pcoinsTip);
        // insightexplorer: update indices (true)
        if (DisconnectBlock(block, state, pindexDelete, view, chainparams, true) != DISCONNECT_OK)
            return error("DisconnectTip(): DisconnectBlock %s failed", pindexDelete->GetBlockHash().ToString());
        assert(view.Flush());
    }
    LogPrint("bench", "- Disconnect block: %.2fms\n", (GetTimeMicros() - nStart) * 0.001);
    uint256 sproutAnchorAfterDisconnect = pcoinsTip->GetBestAnchor(SPROUT);
    uint256 saplingAnchorAfterDisconnect = pcoinsTip->GetBestAnchor(SAPLING);
    // Write the chain state to disk, if necessary.
    if (!FlushStateToDisk(chainparams, state, FLUSH_STATE_IF_NEEDED))
        return false;

    if (!fBare) {
        // Resurrect mempool transactions from the disconnected block.
        for (const CTransaction &tx : block.vtx) {
            // ignore validation errors in resurrected transactions
            list<CTransaction> removed;
            CValidationState stateDummy;
            if (tx.IsCoinBase() || !AcceptToMemoryPool(chainparams, mempool, stateDummy, tx, false, NULL))
                mempool.remove(tx, removed, true);
        }
        if (sproutAnchorBeforeDisconnect != sproutAnchorAfterDisconnect) {
            // The anchor may not change between block disconnects,
            // in which case we don't want to evict from the mempool yet!
            mempool.removeWithAnchor(sproutAnchorBeforeDisconnect, SPROUT);
        }
        if (saplingAnchorBeforeDisconnect != saplingAnchorAfterDisconnect) {
            // The anchor may not change between block disconnects,
            // in which case we don't want to evict from the mempool yet!
            mempool.removeWithAnchor(saplingAnchorBeforeDisconnect, SAPLING);
        }
    }

    // Update chainActive and related variables.
    UpdateTip(pindexDelete->pprev, chainparams);

    // Updates to connected wallets are triggered by ThreadNotifyWallets

    return true;
}

static int64_t nTimeReadFromDisk = 0;
static int64_t nTimeConnectTotal = 0;
static int64_t nTimeFlush = 0;
static int64_t nTimeChainState = 0;
static int64_t nTimePostConnect = 0;

// Protected by cs_main
std::map<CBlockIndex*, std::list<CTransaction>> recentlyConflictedTxs;
uint64_t nRecentlyConflictedSequence = 0;
uint64_t nNotifiedSequence = 0;

/**
 * Connect a new block to chainActive. pblock is either NULL or a pointer to a CBlock
 * corresponding to pindexNew, to bypass loading it again from disk.
 * You probably want to call mempool.removeWithoutBranchId after this, with cs_main held.
 */
bool static ConnectTip(CValidationState& state, const CChainParams& chainparams, CBlockIndex* pindexNew, const CBlock* pblock)
{
    assert(pblock && pindexNew->pprev == chainActive.Tip());
    // Apply the block atomically to the chain state.
    int64_t nTime2 = GetTimeMicros();
    int64_t nTime3;
    {
        CCoinsViewCache view(pcoinsTip);
        bool rv = ConnectBlock(*pblock, state, pindexNew, view, chainparams);
        GetMainSignals().BlockChecked(*pblock, state);
        if (!rv) {
            if (state.IsInvalid())
                InvalidBlockFound(pindexNew, state, chainparams);
            return error("ConnectTip(): ConnectBlock %s failed", pindexNew->GetBlockHash().ToString());
        }
        mapBlockSource.erase(pindexNew->GetBlockHash());
        nTime3 = GetTimeMicros(); nTimeConnectTotal += nTime3 - nTime2;
        LogPrint("bench", "  - Connect total: %.2fms [%.2fs]\n", (nTime3 - nTime2) * 0.001, nTimeConnectTotal * 0.000001);
        assert(view.Flush());
    }
    int64_t nTime4 = GetTimeMicros(); nTimeFlush += nTime4 - nTime3;
    LogPrint("bench", "  - Flush: %.2fms [%.2fs]\n", (nTime4 - nTime3) * 0.001, nTimeFlush * 0.000001);
    // Write the chain state to disk, if necessary.
    if (!FlushStateToDisk(chainparams, state, FLUSH_STATE_IF_NEEDED))
        return false;
    int64_t nTime5 = GetTimeMicros(); nTimeChainState += nTime5 - nTime4;
    LogPrint("bench", "  - Writing chainstate: %.2fms [%.2fs]\n", (nTime5 - nTime4) * 0.001, nTimeChainState * 0.000001);
    // Remove conflicting transactions from the mempool.
    std::list<CTransaction> txConflicted;
    mempool.removeForBlock(pblock->vtx, pindexNew->nHeight, txConflicted, !IsInitialBlockDownload(chainparams.GetConsensus()));

    // Remove transactions that expire at new block height from mempool
    auto ids = mempool.removeExpired(pindexNew->nHeight);

    for (auto id : ids) {
        uiInterface.NotifyTxExpiration(id);
    }

    // Update chainActive & related variables.
    UpdateTip(pindexNew, chainparams);

    // Cache the conflicted transactions for subsequent notification.
    // Updates to connected wallets are triggered by ThreadNotifyWallets
    recentlyConflictedTxs.insert(std::make_pair(pindexNew, txConflicted));
    nRecentlyConflictedSequence += 1;

    EnforceNodeDeprecation(pindexNew->nHeight);

    int64_t nTime6 = GetTimeMicros(); nTimePostConnect += nTime6 - nTime5;
    LogPrint("bench", "  - Connect postprocess: %.2fms [%.2fs]\n", (nTime6 - nTime5) * 0.001, nTimePostConnect * 0.000001);
    // Total connection time benchmarking occurs in ActivateBestChainStep.
    MetricsIncrementCounter("zcash.chain.verified.block.total");
    return true;
}

std::pair<std::map<CBlockIndex*, std::list<CTransaction>>, uint64_t> DrainRecentlyConflicted()
{
    uint64_t recentlyConflictedSequence;
    std::map<CBlockIndex*, std::list<CTransaction>> txs;
    {
        LOCK(cs_main);
        recentlyConflictedSequence = nRecentlyConflictedSequence;
        txs.swap(recentlyConflictedTxs);
    }

    return std::make_pair(txs, recentlyConflictedSequence);
}

void SetChainNotifiedSequence(const CChainParams& chainparams, uint64_t recentlyConflictedSequence) {
    assert(chainparams.NetworkIDString() == "regtest");
    LOCK(cs_main);
    nNotifiedSequence = recentlyConflictedSequence;
}

bool ChainIsFullyNotified(const CChainParams& chainparams) {
    assert(chainparams.NetworkIDString() == "regtest");
    LOCK(cs_main);
    return nRecentlyConflictedSequence == nNotifiedSequence;
}

/**
 * Return the tip of the chain with the most work in it, that isn't
 * known to be invalid (it's however far from certain to be valid).
 */
static CBlockIndex* FindMostWorkChain() {
    do {
        CBlockIndex *pindexNew = NULL;

        // Find the best candidate header.
        {
            std::set<CBlockIndex*, CBlockIndexWorkComparator>::reverse_iterator it = setBlockIndexCandidates.rbegin();
            if (it == setBlockIndexCandidates.rend())
                return NULL;
            pindexNew = *it;
        }

        // Check whether all blocks on the path between the currently active chain and the candidate are valid.
        // Just going until the active chain is an optimization, as we know all blocks in it are valid already.
        CBlockIndex *pindexTest = pindexNew;
        bool fInvalidAncestor = false;
        while (pindexTest && !chainActive.Contains(pindexTest)) {
            assert(pindexTest->nChainTx || pindexTest->nHeight == 0);

            // Pruned nodes may have entries in setBlockIndexCandidates for
            // which block files have been deleted.  Remove those as candidates
            // for the most work chain if we come across them; we can't switch
            // to a chain unless we have all the non-active-chain parent blocks.
            bool fFailedChain = pindexTest->nStatus & BLOCK_FAILED_MASK;
            bool fMissingData = !(pindexTest->nStatus & BLOCK_HAVE_DATA);
            if (fFailedChain || fMissingData) {
                // Candidate chain is not usable (either invalid or missing data)
                if (fFailedChain && (pindexBestInvalid == NULL || pindexNew->nChainWork > pindexBestInvalid->nChainWork))
                    pindexBestInvalid = pindexNew;
                CBlockIndex *pindexFailed = pindexNew;
                // Remove the entire chain from the set.
                while (pindexTest != pindexFailed) {
                    if (fFailedChain) {
                        pindexFailed->nStatus |= BLOCK_FAILED_CHILD;
                    } else if (fMissingData) {
                        // If we're missing data, then add back to mapBlocksUnlinked,
                        // so that if the block arrives in the future we can try adding
                        // to setBlockIndexCandidates again.
                        mapBlocksUnlinked.insert(std::make_pair(pindexFailed->pprev, pindexFailed));
                    }
                    setBlockIndexCandidates.erase(pindexFailed);
                    pindexFailed = pindexFailed->pprev;
                }
                setBlockIndexCandidates.erase(pindexTest);
                fInvalidAncestor = true;
                break;
            }
            pindexTest = pindexTest->pprev;
        }
        if (!fInvalidAncestor)
            return pindexNew;
    } while(true);
}

/** Delete all entries in setBlockIndexCandidates that are worse than the current tip. */
static void PruneBlockIndexCandidates() {
    // Note that we can't delete the current block itself, as we may need to return to it later in case a
    // reorganization to a better block fails.
    std::set<CBlockIndex*, CBlockIndexWorkComparator>::iterator it = setBlockIndexCandidates.begin();
    while (it != setBlockIndexCandidates.end() && setBlockIndexCandidates.value_comp()(*it, chainActive.Tip())) {
        it = setBlockIndexCandidates.erase(it);
    }
    // Either the current tip or a successor of it we're working towards is left in setBlockIndexCandidates.
    assert(!setBlockIndexCandidates.empty());
}

/**
 * Try to make some progress towards making pindexMostWork the active block.
 * pblock is either NULL or a pointer to a CBlock corresponding to pindexMostWork.
 */
static bool ActivateBestChainStep(CValidationState& state, const CChainParams& chainparams, CBlockIndex* pindexMostWork, const CBlock* pblock, bool& fInvalidFound)
{
    AssertLockHeld(cs_main);
    const CBlockIndex *pindexOldTip = chainActive.Tip();
    const CBlockIndex *pindexFork = chainActive.FindFork(pindexMostWork);

    // - On ChainDB initialization, pindexOldTip will be null, so there are no removable blocks.
    // - If pindexMostWork is in a chain that doesn't have the same genesis block as our chain,
    //   then pindexFork will be null, and we would need to remove the entire chain including
    //   our genesis block. In practice this (probably) won't happen because of checks elsewhere.
    auto reorgLength = pindexOldTip ? pindexOldTip->nHeight - (pindexFork ? pindexFork->nHeight : -1) : 0;
    static_assert(MAX_REORG_LENGTH > 0, "We must be able to reorg some distance");
    if (reorgLength > MAX_REORG_LENGTH) {
        auto msg = strprintf(_(
            "A block chain reorganization has been detected that would roll back %d blocks! "
            "This is larger than the maximum of %d blocks, and so the node is shutting down for your safety."
            ), reorgLength, MAX_REORG_LENGTH) + "\n\n" +
            _("Reorganization details") + ":\n" +
            "- " + strprintf(_("Current tip: %s, height %d, work %s"),
                pindexOldTip->phashBlock->GetHex(), pindexOldTip->nHeight, pindexOldTip->nChainWork.GetHex()) + "\n" +
            "- " + strprintf(_("New tip:     %s, height %d, work %s"),
                pindexMostWork->phashBlock->GetHex(), pindexMostWork->nHeight, pindexMostWork->nChainWork.GetHex()) + "\n" +
            "- " + strprintf(_("Fork point:  %s, height %d"),
                pindexFork->phashBlock->GetHex(), pindexFork->nHeight) + "\n\n" +
            _("Please help, human!");
        LogPrintf("*** %s\n", msg);
        uiInterface.ThreadSafeMessageBox(msg, "", CClientUIInterface::MSG_ERROR);
        StartShutdown();
        return false;
    }

    // Disconnect active blocks which are no longer in the best chain.
    bool fBlocksDisconnected = false;
    while (chainActive.Tip() && chainActive.Tip() != pindexFork) {
        if (!DisconnectTip(state, chainparams))
            return false;
        fBlocksDisconnected = true;
    }

    // Build list of new blocks to connect.
    std::vector<CBlockIndex*> vpindexToConnect;
    bool fContinue = true;
    int nHeight = pindexFork ? pindexFork->nHeight : -1;
    while (fContinue && nHeight != pindexMostWork->nHeight) {
        // Don't iterate the entire list of potential improvements toward the best tip, as we likely only need
        // a few blocks along the way.
        int nTargetHeight = std::min(nHeight + 32, pindexMostWork->nHeight);
        vpindexToConnect.clear();
        vpindexToConnect.reserve(nTargetHeight - nHeight);
        CBlockIndex *pindexIter = pindexMostWork->GetAncestor(nTargetHeight);
        while (pindexIter && pindexIter->nHeight != nHeight) {
            vpindexToConnect.push_back(pindexIter);
            pindexIter = pindexIter->pprev;
        }
        nHeight = nTargetHeight;

        // Connect new blocks.
        for (CBlockIndex *pindexConnect : reverse_iterate(vpindexToConnect)) {
            int64_t nTime1 = GetTimeMicros();
            const CBlock* pconnectBlock;
            CBlock block;
            if (pblock && pindexConnect == pindexMostWork) {
                pconnectBlock = pblock;
            } else {
                // read the block to be connected from disk
                if (!ReadBlockFromDisk(block, pindexConnect, chainparams.GetConsensus()))
                    return AbortNode(state, "Failed to read block");
                pconnectBlock = &block;
            }
            int64_t nTime2 = GetTimeMicros(); nTimeReadFromDisk += nTime2 - nTime1;
            LogPrint("bench", "  - Load block from disk: %.2fms [%.2fs]\n", (nTime2 - nTime1) * 0.001, nTimeReadFromDisk * 0.000001);

            if (!ConnectTip(state, chainparams, pindexConnect, pconnectBlock)) {
                if (state.IsInvalid()) {
                    // The block violates a consensus rule.
                    if (!state.CorruptionPossible())
                        InvalidChainFound(vpindexToConnect.back(), chainparams);
                    state = CValidationState();
                    fInvalidFound = true;
                    fContinue = false;
                    break;
                } else {
                    // A system error occurred (disk space, database error, ...).
                    return false;
                }
            } else {
                int64_t nTime3 = GetTimeMicros(); nTimeTotal += nTime3 - nTime1;
                LogPrint("bench", "- Connect block: %.2fms [%.2fs]\n", (nTime3 - nTime1) * 0.001, nTimeTotal * 0.000001);
                MetricsHistogram("zcash.chain.verified.block.seconds", (nTime3 - nTime1) * 0.000001);

                PruneBlockIndexCandidates();
                if (!pindexOldTip || chainActive.Tip()->nChainWork > pindexOldTip->nChainWork) {
                    // We're in a better position than we were. Return temporarily to release the lock.
                    fContinue = false;
                    break;
                }
            }
        }
    }

    if (fBlocksDisconnected) {
        mempool.removeForReorg(pcoinsTip, chainActive.Tip()->nHeight + 1, STANDARD_LOCKTIME_VERIFY_FLAGS);
    }
    mempool.removeWithoutBranchId(
        CurrentEpochBranchId(chainActive.Tip()->nHeight + 1, chainparams.GetConsensus()));
    mempool.check(pcoinsTip);

    // Callbacks/notifications for a new best chain.
    if (fInvalidFound)
        CheckForkWarningConditionsOnNewFork(vpindexToConnect.back(), chainparams);
    else
        CheckForkWarningConditions(chainparams.GetConsensus());

    return true;
}

static void NotifyHeaderTip(const Consensus::Params& params) {
    bool fNotify = false;
    bool fInitialBlockDownload = false;
    static CBlockIndex* pindexHeaderOld = NULL;
    CBlockIndex* pindexHeader = NULL;
    {
        LOCK(cs_main);
        if (!setBlockIndexCandidates.empty()) {
            pindexHeader = *setBlockIndexCandidates.rbegin();
        }
        if (pindexHeader != pindexHeaderOld) {
            fNotify = true;
            fInitialBlockDownload = IsInitialBlockDownload(params);
            pindexHeaderOld = pindexHeader;
        }
    }
    // Send block tip changed notifications without cs_main
    if (fNotify) {
        uiInterface.NotifyHeaderTip(fInitialBlockDownload, pindexHeader);
    }
}

/**
 * Make the best chain active, in multiple steps. The result is either failure
 * or an activated best chain. pblock is either NULL or a pointer to a block
 * that is already loaded (to avoid loading it again from disk).
 */
bool ActivateBestChain(CValidationState& state, const CChainParams& chainparams, const CBlock* pblock)
{
    CBlockIndex *pindexMostWork = NULL;
    CBlockIndex *pindexNewTip = NULL;
    do {
        boost::this_thread::interruption_point();

        bool fInitialDownload;
        int nNewHeight;
        {
            LOCK(cs_main);
            if (pindexMostWork == NULL) {
                pindexMostWork = FindMostWorkChain();
            }

            // Whether we have anything to do at all.
            if (pindexMostWork == NULL || pindexMostWork == chainActive.Tip())
                return true;

            bool fInvalidFound = false;
            if (!ActivateBestChainStep(state, chainparams, pindexMostWork, pblock && pblock->GetHash() == pindexMostWork->GetBlockHash() ? pblock : NULL, fInvalidFound))
                return false;

            if (fInvalidFound) {
                // Wipe cache, we may need another branch now.
                pindexMostWork = NULL;
            }
            pindexNewTip = chainActive.Tip();
            fInitialDownload = IsInitialBlockDownload(chainparams.GetConsensus());
            nNewHeight = chainActive.Height();
        }
        // When we reach this point, we switched to a new tip (stored in pindexNewTip).

        // Notifications/callbacks that can run without cs_main
        // Always notify the UI if a new block tip was connected
        uiInterface.NotifyBlockTip(fInitialDownload, pindexNewTip);
        if (!fInitialDownload) {
            uint256 hashNewTip = pindexNewTip->GetBlockHash();
            // Relay inventory, but don't relay old inventory during initial block download.
            int nBlockEstimate = 0;
            if (fCheckpointsEnabled)
                nBlockEstimate = Checkpoints::GetTotalBlocksEstimate(chainparams.Checkpoints());
            {
                LOCK(cs_vNodes);
                for (CNode* pnode : vNodes)
                    if (nNewHeight > (pnode->nStartingHeight != -1 ? pnode->nStartingHeight - 2000 : nBlockEstimate))
                        pnode->PushInventory(CInv(MSG_BLOCK, hashNewTip));
            }
            // Notify external listeners about the new tip.
            GetMainSignals().UpdatedBlockTip(pindexNewTip);
        }
    } while (pindexNewTip != pindexMostWork);
    CheckBlockIndex(chainparams.GetConsensus());

    // Write changes periodically to disk, after relay.
    if (!FlushStateToDisk(chainparams, state, FLUSH_STATE_PERIODIC)) {
        return false;
    }

    return true;
}

bool InvalidateBlock(CValidationState& state, const CChainParams& chainparams, CBlockIndex *pindex)
{
    AssertLockHeld(cs_main);

    // Mark the block itself as invalid.
    pindex->nStatus |= BLOCK_FAILED_VALID;
    setDirtyBlockIndex.insert(pindex);
    setBlockIndexCandidates.erase(pindex);

    while (chainActive.Contains(pindex)) {
        CBlockIndex *pindexWalk = chainActive.Tip();
        pindexWalk->nStatus |= BLOCK_FAILED_CHILD;
        setDirtyBlockIndex.insert(pindexWalk);
        setBlockIndexCandidates.erase(pindexWalk);
        // ActivateBestChain considers blocks already in chainActive
        // unconditionally valid already, so force disconnect away from it.
        if (!DisconnectTip(state, chainparams)) {
            mempool.removeForReorg(pcoinsTip, chainActive.Tip()->nHeight + 1, STANDARD_LOCKTIME_VERIFY_FLAGS);
            mempool.removeWithoutBranchId(
                CurrentEpochBranchId(chainActive.Tip()->nHeight + 1, chainparams.GetConsensus()));
            return false;
        }
    }

    // The resulting new best tip may not be in setBlockIndexCandidates anymore, so
    // add it again.
    BlockMap::iterator it = mapBlockIndex.begin();
    while (it != mapBlockIndex.end()) {
        if (it->second->IsValid(BLOCK_VALID_TRANSACTIONS) && it->second->nChainTx && !setBlockIndexCandidates.value_comp()(it->second, chainActive.Tip())) {
            setBlockIndexCandidates.insert(it->second);
        }
        it++;
    }

    InvalidChainFound(pindex, chainparams);
    mempool.removeForReorg(pcoinsTip, chainActive.Tip()->nHeight + 1, STANDARD_LOCKTIME_VERIFY_FLAGS);
    mempool.removeWithoutBranchId(
        CurrentEpochBranchId(chainActive.Tip()->nHeight + 1, chainparams.GetConsensus()));
    return true;
}

bool ReconsiderBlock(CValidationState& state, CBlockIndex *pindex) {
    AssertLockHeld(cs_main);

    int nHeight = pindex->nHeight;

    // Remove the invalidity flag from this block and all its descendants.
    BlockMap::iterator it = mapBlockIndex.begin();
    while (it != mapBlockIndex.end()) {
        if (!it->second->IsValid() && it->second->GetAncestor(nHeight) == pindex) {
            it->second->nStatus &= ~BLOCK_FAILED_MASK;
            setDirtyBlockIndex.insert(it->second);
            if (it->second->IsValid(BLOCK_VALID_TRANSACTIONS) && it->second->nChainTx && setBlockIndexCandidates.value_comp()(chainActive.Tip(), it->second)) {
                setBlockIndexCandidates.insert(it->second);
            }
            if (it->second == pindexBestInvalid) {
                // Reset invalid block marker if it was pointing to one of those.
                pindexBestInvalid = NULL;
            }
        }
        it++;
    }

    // Remove the invalidity flag from all ancestors too.
    while (pindex != NULL) {
        if (pindex->nStatus & BLOCK_FAILED_MASK) {
            pindex->nStatus &= ~BLOCK_FAILED_MASK;
            setDirtyBlockIndex.insert(pindex);
        }
        pindex = pindex->pprev;
    }
    return true;
}

CBlockIndex* AddToBlockIndex(const CBlockHeader& block, const Consensus::Params& consensusParams)
{
    // Check for duplicate
    uint256 hash = block.GetHash();
    BlockMap::iterator it = mapBlockIndex.find(hash);
    if (it != mapBlockIndex.end())
        return it->second;

    // Construct new block index object
    CBlockIndex* pindexNew = new CBlockIndex(block);
    assert(pindexNew);
    // We assign the sequence id to blocks only when the full data is available,
    // to avoid miners withholding blocks but broadcasting headers, to get a
    // competitive advantage.
    pindexNew->nSequenceId = 0;
    BlockMap::iterator mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);
    BlockMap::iterator miPrev = mapBlockIndex.find(block.hashPrevBlock);
    if (miPrev != mapBlockIndex.end())
    {
        pindexNew->pprev = (*miPrev).second;
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;

        if (consensusParams.NetworkUpgradeActive(pindexNew->nHeight, Consensus::UPGRADE_NU5)) {
            // The following hashes will be null if this block has never been
            // connected to a main chain; they will be (re)set correctly in
            // ConnectBlock:
            // - hashFinalSaplingRoot
            // - hashFinalOrchardRoot
            // - hashChainHistoryRoot
        } else if (IsActivationHeight(pindexNew->nHeight, consensusParams, Consensus::UPGRADE_HEARTWOOD)) {
            // hashFinalSaplingRoot and hashFinalOrchardRoot will be null if this block has
            // never been connected to a main chain; they will be (re)set correctly in
            // ConnectBlock.
            // hashChainHistoryRoot is null.
        } else if (consensusParams.NetworkUpgradeActive(pindexNew->nHeight, Consensus::UPGRADE_HEARTWOOD)) {
            // hashFinalSaplingRoot and hashFinalOrchardRoot will be null if this block has
            // never been connected to a main chain; they will be (re)set correctly in
            // ConnectBlock.
            pindexNew->hashChainHistoryRoot = pindexNew->hashBlockCommitments;
        } else {
            // hashFinalOrchardRoot and hashChainHistoryRoot are null.
            pindexNew->hashFinalSaplingRoot = pindexNew->hashBlockCommitments;
        }

        pindexNew->BuildSkip();
    }
    pindexNew->nChainWork = (pindexNew->pprev ? pindexNew->pprev->nChainWork : 0) + GetBlockProof(*pindexNew);
    pindexNew->RaiseValidity(BLOCK_VALID_TREE);
    if (pindexBestHeader == NULL || pindexBestHeader->nChainWork < pindexNew->nChainWork)
        pindexBestHeader = pindexNew;

    setDirtyBlockIndex.insert(pindexNew);

    return pindexNew;
}

void FallbackSproutValuePoolBalance(
    CBlockIndex *pindex,
    const CChainParams& chainparams
)
{
    if (!chainparams.ZIP209Enabled()) {
        return;
    }

    // When developer option -developersetpoolsizezero is enabled, we don't need a fallback balance.
    if (fExperimentalDeveloperSetPoolSizeZero) {
        return;
    }

    // Check if the height of this block matches the checkpoint
    if (pindex->nHeight == chainparams.SproutValuePoolCheckpointHeight()) {
        if (pindex->GetBlockHash() == chainparams.SproutValuePoolCheckpointBlockHash()) {
            // Are we monitoring the Sprout pool?
            if (!pindex->nChainSproutValue) {
                // Apparently not. Introduce the hardcoded value so we monitor for
                // this point onwards (assuming the checkpoint is late enough)
                pindex->nChainSproutValue = chainparams.SproutValuePoolCheckpointBalance();
            } else {
                // Apparently we have been. So, we should expect the current
                // value to match the hardcoded one.
                assert(*pindex->nChainSproutValue == chainparams.SproutValuePoolCheckpointBalance());
                // And we should expect non-none for the delta stored in the block index here,
                // or the checkpoint is too early.
                assert(pindex->nSproutValue != std::nullopt);
            }
        } else {
            LogPrintf(
                "FallbackSproutValuePoolBalance(): fallback block hash is incorrect, we got %s\n",
                pindex->GetBlockHash().ToString()
            );
        }
    }
}

/** Mark a block as having its data received and checked (up to BLOCK_VALID_TRANSACTIONS). */
bool ReceivedBlockTransactions(
    const CBlock &block,
    CValidationState& state,
    const CChainParams& chainparams,
    CBlockIndex *pindexNew,
    const CDiskBlockPos& pos)
{
    pindexNew->nTx = block.vtx.size();
    pindexNew->nChainTx = 0;
    CAmount sproutValue = 0;
    CAmount saplingValue = 0;
    CAmount orchardValue = 0;
    for (auto tx : block.vtx) {
        // Negative valueBalanceSapling "takes" money from the transparent value pool
        // and adds it to the Sapling value pool. Positive valueBalanceSapling "gives"
        // money to the transparent value pool, removing from the Sapling value
        // pool. So we invert the sign here.
        saplingValue += -tx.GetValueBalanceSapling();

        // valueBalanceOrchard behaves the same way as valueBalanceSapling.
        orchardValue += -tx.GetOrchardBundle().GetValueBalance();

        for (auto js : tx.vJoinSplit) {
            sproutValue += js.vpub_old;
            sproutValue -= js.vpub_new;
        }
    }
    pindexNew->nSproutValue = sproutValue;
    pindexNew->nChainSproutValue = std::nullopt;
    pindexNew->nSaplingValue = saplingValue;
    pindexNew->nChainSaplingValue = std::nullopt;
    pindexNew->nOrchardValue = orchardValue;
    pindexNew->nChainOrchardValue = std::nullopt;
    pindexNew->nFile = pos.nFile;
    pindexNew->nDataPos = pos.nPos;
    pindexNew->nUndoPos = 0;
    pindexNew->nStatus |= BLOCK_HAVE_DATA;
    pindexNew->RaiseValidity(BLOCK_VALID_TRANSACTIONS);
    setDirtyBlockIndex.insert(pindexNew);

    if (pindexNew->pprev == NULL || pindexNew->pprev->nChainTx) {
        // If pindexNew is the genesis block or all parents are BLOCK_VALID_TRANSACTIONS.
        deque<CBlockIndex*> queue;
        queue.push_back(pindexNew);

        // Recursively process any descendant blocks that now may be eligible to be connected.
        while (!queue.empty()) {
            CBlockIndex *pindex = queue.front();
            queue.pop_front();
            pindex->nChainTx = (pindex->pprev ? pindex->pprev->nChainTx : 0) + pindex->nTx;
            if (pindex->pprev) {
                if (pindex->pprev->nChainSproutValue && pindex->nSproutValue) {
                    pindex->nChainSproutValue = *pindex->pprev->nChainSproutValue + *pindex->nSproutValue;
                } else {
                    pindex->nChainSproutValue = std::nullopt;
                }
                if (pindex->pprev->nChainSaplingValue) {
                    pindex->nChainSaplingValue = *pindex->pprev->nChainSaplingValue + pindex->nSaplingValue;
                } else {
                    pindex->nChainSaplingValue = std::nullopt;
                }
                if (pindex->pprev->nChainOrchardValue) {
                    pindex->nChainOrchardValue = *pindex->pprev->nChainOrchardValue + pindex->nOrchardValue;
                } else {
                    pindex->nChainOrchardValue = std::nullopt;
                }
            } else {
                pindex->nChainSproutValue = pindex->nSproutValue;
                pindex->nChainSaplingValue = pindex->nSaplingValue;
                pindex->nChainOrchardValue = pindex->nOrchardValue;
            }

            // Fall back to hardcoded Sprout value pool balance
            FallbackSproutValuePoolBalance(pindex, chainparams);

            {
                LOCK(cs_nBlockSequenceId);
                pindex->nSequenceId = nBlockSequenceId++;
            }
            if (chainActive.Tip() == NULL || !setBlockIndexCandidates.value_comp()(pindex, chainActive.Tip())) {
                setBlockIndexCandidates.insert(pindex);
            }
            std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> range = mapBlocksUnlinked.equal_range(pindex);
            while (range.first != range.second) {
                queue.push_back(range.first->second);
                range.first = mapBlocksUnlinked.erase(range.first);
            }
        }
    } else {
        if (pindexNew->pprev && pindexNew->pprev->IsValid(BLOCK_VALID_TREE)) {
            mapBlocksUnlinked.insert(std::make_pair(pindexNew->pprev, pindexNew));
        }
    }

    return true;
}

bool FindBlockPos(CValidationState &state, CDiskBlockPos &pos, unsigned int nAddSize, unsigned int nHeight, uint64_t nTime, bool fKnown = false)
{
    LOCK(cs_LastBlockFile);

    unsigned int nFile = fKnown ? pos.nFile : nLastBlockFile;
    if (vinfoBlockFile.size() <= nFile) {
        vinfoBlockFile.resize(nFile + 1);
    }

    if (!fKnown) {
        while (vinfoBlockFile[nFile].nSize + nAddSize >= MAX_BLOCKFILE_SIZE) {
            nFile++;
            if (vinfoBlockFile.size() <= nFile) {
                vinfoBlockFile.resize(nFile + 1);
            }
        }
        pos.nFile = nFile;
        pos.nPos = vinfoBlockFile[nFile].nSize;
    }

    if (nFile != nLastBlockFile) {
        if (!fKnown) {
            LogPrintf("Leaving block file %i: %s\n", nFile, vinfoBlockFile[nFile].ToString());
        }
        FlushBlockFile(!fKnown);
        nLastBlockFile = nFile;
    }

    vinfoBlockFile[nFile].AddBlock(nHeight, nTime);
    if (fKnown)
        vinfoBlockFile[nFile].nSize = std::max(pos.nPos + nAddSize, vinfoBlockFile[nFile].nSize);
    else
        vinfoBlockFile[nFile].nSize += nAddSize;

    if (!fKnown) {
        unsigned int nOldChunks = (pos.nPos + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
        unsigned int nNewChunks = (vinfoBlockFile[nFile].nSize + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
        if (nNewChunks > nOldChunks) {
            if (fPruneMode)
                fCheckForPruning = true;
            if (CheckDiskSpace(nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos)) {
                FILE *file = OpenBlockFile(pos);
                if (file) {
                    LogPrintf("Pre-allocating up to position 0x%x in blk%05u.dat\n", nNewChunks * BLOCKFILE_CHUNK_SIZE, pos.nFile);
                    AllocateFileRange(file, pos.nPos, nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos);
                    fclose(file);
                }
            }
            else
                return state.Error("out of disk space");
        }
    }

    setDirtyFileInfo.insert(nFile);
    return true;
}

bool FindUndoPos(CValidationState &state, int nFile, CDiskBlockPos &pos, unsigned int nAddSize)
{
    pos.nFile = nFile;

    LOCK(cs_LastBlockFile);

    unsigned int nNewSize;
    pos.nPos = vinfoBlockFile[nFile].nUndoSize;
    nNewSize = vinfoBlockFile[nFile].nUndoSize += nAddSize;
    setDirtyFileInfo.insert(nFile);

    unsigned int nOldChunks = (pos.nPos + UNDOFILE_CHUNK_SIZE - 1) / UNDOFILE_CHUNK_SIZE;
    unsigned int nNewChunks = (nNewSize + UNDOFILE_CHUNK_SIZE - 1) / UNDOFILE_CHUNK_SIZE;
    if (nNewChunks > nOldChunks) {
        if (fPruneMode)
            fCheckForPruning = true;
        if (CheckDiskSpace(nNewChunks * UNDOFILE_CHUNK_SIZE - pos.nPos)) {
            FILE *file = OpenUndoFile(pos);
            if (file) {
                LogPrintf("Pre-allocating up to position 0x%x in rev%05u.dat\n", nNewChunks * UNDOFILE_CHUNK_SIZE, pos.nFile);
                AllocateFileRange(file, pos.nPos, nNewChunks * UNDOFILE_CHUNK_SIZE - pos.nPos);
                fclose(file);
            }
        }
        else
            return state.Error("out of disk space");
    }

    return true;
}

bool CheckBlockHeader(
    const CBlockHeader& block,
    CValidationState& state,
    const CChainParams& chainparams,
    bool fCheckPOW)
{
    // Check block version
    if (block.nVersion < MIN_BLOCK_VERSION)
        return state.DoS(100, error("CheckBlockHeader(): block version too low"),
                         REJECT_INVALID, "version-too-low");

    // Check Equihash solution is valid
    if (fCheckPOW && !CheckEquihashSolution(&block, chainparams.GetConsensus()))
        return state.DoS(100, error("CheckBlockHeader(): Equihash solution invalid"),
                         REJECT_INVALID, "invalid-solution");

    // Check proof of work matches claimed amount
    if (fCheckPOW && !CheckProofOfWork(block.GetHash(), block.nBits, chainparams.GetConsensus()))
        return state.DoS(50, error("CheckBlockHeader(): proof of work failed"),
                         REJECT_INVALID, "high-hash");

    return true;
}

bool CheckBlock(const CBlock& block,
                CValidationState& state,
                const CChainParams& chainparams,
                ProofVerifier& verifier,
                orchard::AuthValidator& orchardAuth,
                bool fCheckPOW,
                bool fCheckMerkleRoot,
                bool fCheckTransactions)
{
    // These are checks that are independent of context.

    // Check that the header is valid (particularly PoW).  This is mostly
    // redundant with the call in AcceptBlockHeader.
    if (!CheckBlockHeader(block, state, chainparams, fCheckPOW))
        return false;

    // Check the merkle root.
    if (fCheckMerkleRoot) {
        bool mutated;
        uint256 hashMerkleRoot2 = block.BuildMerkleTree(&mutated);
        if (block.hashMerkleRoot != hashMerkleRoot2)
            return state.DoS(100, error("CheckBlock(): hashMerkleRoot mismatch"),
                             REJECT_INVALID, "bad-txnmrklroot", true);

        // Check for merkle tree malleability (CVE-2012-2459): repeating sequences
        // of transactions in a block without affecting the merkle root of a block,
        // while still invalidating it.
        if (mutated)
            return state.DoS(100, error("CheckBlock(): duplicate transaction"),
                             REJECT_INVALID, "bad-txns-duplicate", true);
    }

    // All potential-corruption validation must be done before we do any
    // transaction validation, as otherwise we may mark the header as invalid
    // because we receive the wrong transactions for it.

    // Size limits
    if (block.vtx.empty() || block.vtx.size() > MAX_BLOCK_SIZE || ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION) > MAX_BLOCK_SIZE)
        return state.DoS(100, error("CheckBlock(): size limits failed"),
                         REJECT_INVALID, "bad-blk-length");

    // First transaction must be coinbase, the rest must not be
    if (block.vtx.empty() || !block.vtx[0].IsCoinBase())
        return state.DoS(100, error("CheckBlock(): first tx is not coinbase"),
                         REJECT_INVALID, "bad-cb-missing");
    for (unsigned int i = 1; i < block.vtx.size(); i++)
        if (block.vtx[i].IsCoinBase())
            return state.DoS(100, error("CheckBlock(): more than one coinbase"),
                             REJECT_INVALID, "bad-cb-multiple");

    // skip all transaction checks if this flag is not set
    if (!fCheckTransactions) return true;

    // Check transactions
    for (const CTransaction& tx : block.vtx)
        if (!CheckTransaction(tx, state, verifier, orchardAuth))
            return error("CheckBlock(): CheckTransaction failed");

    unsigned int nSigOps = 0;
    for (const CTransaction& tx : block.vtx)
    {
        nSigOps += GetLegacySigOpCount(tx);
    }
    if (nSigOps > MAX_BLOCK_SIGOPS)
        return state.DoS(100, error("CheckBlock(): out-of-bounds SigOpCount"),
                         REJECT_INVALID, "bad-blk-sigops", true);

    return true;
}

bool ContextualCheckBlockHeader(
    const CBlockHeader& block, CValidationState& state,
    const CChainParams& chainParams, CBlockIndex * const pindexPrev)
{
    const Consensus::Params& consensusParams = chainParams.GetConsensus();
    uint256 hash = block.GetHash();
    if (hash == consensusParams.hashGenesisBlock) {
        return true;
    }

    assert(pindexPrev);

    int nHeight = pindexPrev->nHeight+1;

    // Check proof of work
    if (block.nBits != GetNextWorkRequired(pindexPrev, &block, consensusParams)) {
        return state.DoS(100, error("%s: incorrect proof of work", __func__),
                         REJECT_INVALID, "bad-diffbits");
    }

    // Check timestamp against prev
    auto medianTimePast = pindexPrev->GetMedianTimePast();
    if (block.GetBlockTime() <= medianTimePast) {
        return state.Invalid(error("%s: block at height %d, timestamp %d is not later than median-time-past %d",
                                   __func__, nHeight, block.GetBlockTime(), medianTimePast),
                             REJECT_INVALID, "time-too-old");
    }

    // Check future timestamp soft fork rule introduced in v2.1.1-1.
    // This retrospectively activates at block height 2 for mainnet and regtest,
    // and 6 blocks after Blossom activation for testnet.
    // Explanations of these activation heights are in src/consensus/params.h
    // and chainparams.cpp.
    //
    if (consensusParams.FutureTimestampSoftForkActive(nHeight) &&
          block.GetBlockTime() > medianTimePast + MAX_FUTURE_BLOCK_TIME_MTP) {
        return state.Invalid(error("%s: block at height %d, timestamp %d is too far ahead of median-time-past, limit is %d",
                                   __func__, nHeight, block.GetBlockTime(), medianTimePast + MAX_FUTURE_BLOCK_TIME_MTP),
                             REJECT_INVALID, "time-too-far-ahead-of-mtp");
    }

    // Check timestamp
    auto nTimeLimit = GetTime() + MAX_FUTURE_BLOCK_TIME_LOCAL;
    if (block.GetBlockTime() > nTimeLimit) {
        return state.Invalid(error("%s: block at height %d, timestamp %d is too far ahead of local time, limit is %d",
                                   __func__, nHeight, block.GetBlockTime(), nTimeLimit),
                             REJECT_INVALID, "time-too-new");
    }

    if (fCheckpointsEnabled) {
        // Don't accept any forks from the main chain prior to last checkpoint
        CBlockIndex* pcheckpoint = Checkpoints::GetLastCheckpoint(chainParams.Checkpoints());
        if (pcheckpoint && nHeight < pcheckpoint->nHeight) {
            return state.DoS(100, error("%s: forked chain older than last checkpoint (height %d)", __func__, nHeight));
        }
    }

    // Reject block.nVersion < 4 blocks
    if (block.nVersion < 4) {
        return state.Invalid(error("%s : rejected nVersion<4 block", __func__),
                             REJECT_OBSOLETE, "bad-version");
    }

    return true;
}

bool ContextualCheckBlock(
    const CBlock& block, CValidationState& state,
    const CChainParams& chainparams, CBlockIndex * const pindexPrev,
    bool fCheckTransactions)
{
    const int nHeight = pindexPrev == NULL ? 0 : pindexPrev->nHeight + 1;
    const Consensus::Params& consensusParams = chainparams.GetConsensus();

    if (fCheckTransactions) {
        // Check that all transactions are finalized
        for (const CTransaction& tx : block.vtx) {

            // Check transaction contextually against consensus rules at block height
            if (!ContextualCheckTransaction(tx, state, chainparams, nHeight, true)) {
                return false; // Failure reason has been set in validation state object
            }

            int nLockTimeFlags = 0;
            int64_t nLockTimeCutoff = (nLockTimeFlags & LOCKTIME_MEDIAN_TIME_PAST)
                                    ? pindexPrev->GetMedianTimePast()
                                    : block.GetBlockTime();
            if (!IsFinalTx(tx, nHeight, nLockTimeCutoff)) {
                return state.DoS(10, error("%s: contains a non-final transaction", __func__),
                                 REJECT_INVALID, "bad-txns-nonfinal");
            }
        }
    }

    // Enforce BIP 34 rule that the coinbase starts with serialized block height.
    // In Zcash this has been enforced since launch, except that the genesis
    // block didn't include the height in the coinbase (see Zcash protocol spec
    // section '6.8 Bitcoin Improvement Proposals').
    if (nHeight > 0)
    {
        CScript expect = CScript() << nHeight;
        if (block.vtx[0].vin[0].scriptSig.size() < expect.size() ||
            !std::equal(expect.begin(), expect.end(), block.vtx[0].vin[0].scriptSig.begin())) {
            return state.DoS(100, error("%s: block height mismatch in coinbase", __func__),
                             REJECT_INVALID, "bad-cb-height");
        }
    }

    // ZIP 203: From NU5 onwards, nExpiryHeight is set to the block height in coinbase
    // transactions.
    if (consensusParams.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_NU5)) {
        if (block.vtx[0].nExpiryHeight != nHeight) {
            return state.DoS(100, error("%s: block height mismatch in nExpiryHeight", __func__),
                             REJECT_INVALID, "bad-cb-height");
        }
    }

    if (consensusParams.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_CANOPY)) {
        // Funding streams are checked inside ContextualCheckTransaction.
        // This empty conditional branch exists to enforce this ZIP 207 consensus rule:
        //
        //     Once the Canopy network upgrade activates, the existing consensus rule for
        //     payment of the Founders' Reward is no longer active.
    } else if ((nHeight > 0) && (nHeight <= consensusParams.GetLastFoundersRewardBlockHeight(nHeight))) {
        // Coinbase transaction must include an output sending 20% of
        // the block subsidy to a Founders' Reward script, until the last Founders'
        // Reward block is reached, with exception of the genesis block.
        // The last Founders' Reward block is defined as the block just before the
        // first subsidy halving block, which occurs at halving_interval + slow_start_shift.
        bool found = false;

        for (const CTxOut& output : block.vtx[0].vout) {
            if (output.scriptPubKey == chainparams.GetFoundersRewardScriptAtHeight(nHeight)) {
                if (output.nValue == (GetBlockSubsidy(nHeight, consensusParams) / 5)) {
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            return state.DoS(100, error("%s: founders reward missing", __func__),
                             REJECT_INVALID, "cb-no-founders-reward");
        }
    }

    return true;
}

static bool AcceptBlockHeader(const CBlockHeader& block, CValidationState& state, const CChainParams& chainparams, CBlockIndex** ppindex=NULL)
{
    AssertLockHeld(cs_main);
    // Check for duplicate
    uint256 hash = block.GetHash();
    BlockMap::iterator miSelf = mapBlockIndex.find(hash);
    CBlockIndex *pindex = NULL;
    if (miSelf != mapBlockIndex.end()) {
        // Block header is already known.
        pindex = miSelf->second;
        if (ppindex)
            *ppindex = pindex;
        if (pindex->nStatus & BLOCK_FAILED_MASK)
            return state.Invalid(error("%s: block is marked invalid", __func__), 0, "duplicate");
        return true;
    }

    if (!CheckBlockHeader(block, state, chainparams))
        return false;

    // Get prev block index
    CBlockIndex* pindexPrev = NULL;
    if (hash != chainparams.GetConsensus().hashGenesisBlock) {
        BlockMap::iterator mi = mapBlockIndex.find(block.hashPrevBlock);
        if (mi == mapBlockIndex.end())
            return state.DoS(10, error("%s: prev block not found", __func__), 0, "bad-prevblk");
        pindexPrev = (*mi).second;
        if (pindexPrev->nStatus & BLOCK_FAILED_MASK)
            return state.DoS(100, error("%s: prev block invalid", __func__), REJECT_INVALID, "bad-prevblk");
    }

    if (!ContextualCheckBlockHeader(block, state, chainparams, pindexPrev))
        return false;

    if (pindex == NULL)
        pindex = AddToBlockIndex(block, chainparams.GetConsensus());

    if (ppindex)
        *ppindex = pindex;

    return true;
}

/**
 * Store block on disk.
 * If dbp is non-NULL, the file is known to already reside on disk.
 *
 * JoinSplit proofs and Orchard authorizations are not verified here; the only
 * caller of AcceptBlock (ProcessNewBlock) later invokes ActivateBestChain,
 * which ultimately calls ConnectBlock in a manner that can verify the proofs.
 */
static bool AcceptBlock(const CBlock& block, CValidationState& state, const CChainParams& chainparams, CBlockIndex** ppindex, bool fRequested, const CDiskBlockPos* dbp)
{
    AssertLockHeld(cs_main);

    CBlockIndex *pindexDummy = NULL;
    CBlockIndex *&pindex = ppindex ? *ppindex : pindexDummy;

    if (!AcceptBlockHeader(block, state, chainparams, &pindex))
        return false;

    // Try to process all requested blocks that we don't have, but only
    // process an unrequested block if it's new and has enough work to
    // advance our tip, and isn't too many blocks ahead.
    bool fAlreadyHave = pindex->nStatus & BLOCK_HAVE_DATA;
    bool fHasMoreWork = (chainActive.Tip() ? pindex->nChainWork > chainActive.Tip()->nChainWork : true);
    // Blocks that are too out-of-order needlessly limit the effectiveness of
    // pruning, because pruning will not delete block files that contain any
    // blocks which are too close in height to the tip.  Apply this test
    // regardless of whether pruning is enabled; it should generally be safe to
    // not process unrequested blocks.
    bool fTooFarAhead = (pindex->nHeight > int(chainActive.Height() + MIN_BLOCKS_TO_KEEP));

    // TODO: deal better with return value and error conditions for duplicate
    // and unrequested blocks.
    if (fAlreadyHave) return true;
    if (!fRequested) {  // If we didn't ask for it:
        if (pindex->nTx != 0) return true;  // This is a previously-processed block that was pruned
        if (!fHasMoreWork) return true;     // Don't process less-work chains
        if (fTooFarAhead) return true;      // Block height is too high
    }

    // See method docstring for why these are always disabled.
    auto verifier = ProofVerifier::Disabled();
    auto orchardAuth = orchard::AuthValidator::Disabled();

    bool fCheckTransactions = ShouldCheckTransactions(chainparams, pindex);
    if ((!CheckBlock(block, state, chainparams, verifier, orchardAuth, true, true, fCheckTransactions)) ||
         !ContextualCheckBlock(block, state, chainparams, pindex->pprev, fCheckTransactions)) {
        if (state.IsInvalid() && !state.CorruptionPossible()) {
            pindex->nStatus |= BLOCK_FAILED_VALID;
            setDirtyBlockIndex.insert(pindex);
        }
        return false;
    }

    int nHeight = pindex->nHeight;

    // Write block to history file
    try {
        unsigned int nBlockSize = ::GetSerializeSize(block, SER_DISK, CLIENT_VERSION);
        CDiskBlockPos blockPos;
        if (dbp != NULL)
            blockPos = *dbp;
        if (!FindBlockPos(state, blockPos, nBlockSize+8, nHeight, block.GetBlockTime(), dbp != NULL))
            return error("AcceptBlock(): FindBlockPos failed");
        if (dbp == NULL)
            if (!WriteBlockToDisk(block, blockPos, chainparams.MessageStart()))
                AbortNode(state, "Failed to write block");
        if (!ReceivedBlockTransactions(block, state, chainparams, pindex, blockPos))
            return error("AcceptBlock(): ReceivedBlockTransactions failed");
    } catch (const std::runtime_error& e) {
        return AbortNode(state, std::string("System error: ") + e.what());
    }

    if (fCheckForPruning)
        FlushStateToDisk(chainparams, state, FLUSH_STATE_NONE); // we just allocated more disk space for block files

    return true;
}

static bool IsSuperMajority(int minVersion, const CBlockIndex* pstart, unsigned nRequired, const Consensus::Params& consensusParams)
{
    unsigned int nFound = 0;
    for (int i = 0; i < consensusParams.nMajorityWindow && nFound < nRequired && pstart != NULL; i++)
    {
        if (pstart->nVersion >= minVersion)
            ++nFound;
        pstart = pstart->pprev;
    }
    return (nFound >= nRequired);
}


bool ProcessNewBlock(CValidationState& state, const CChainParams& chainparams, const CNode* pfrom, const CBlock* pblock, bool fForceProcessing, const CDiskBlockPos* dbp)
{
    auto span = TracingSpan("info", "main", "ProcessNewBlock");
    auto spanGuard = span.Enter();

    {
        LOCK(cs_main);
        bool fRequested = MarkBlockAsReceived(pblock->GetHash()) | fForceProcessing;

        // Store to disk
        CBlockIndex *pindex = NULL;
        bool ret = AcceptBlock(*pblock, state, chainparams, &pindex, fRequested, dbp);
        if (pindex && pfrom) {
            mapBlockSource[pindex->GetBlockHash()] = pfrom->GetId();
        }
        CheckBlockIndex(chainparams.GetConsensus());
        if (!ret)
            return error("%s: AcceptBlock FAILED", __func__);
    }

    NotifyHeaderTip(chainparams.GetConsensus());

    if (!ActivateBestChain(state, chainparams, pblock))
        return error("%s: ActivateBestChain failed", __func__);

    return true;
}

/**
 * This is only invoked by the miner.
 * The block's proof-of-work is assumed invalid and not checked.
 */
bool TestBlockValidity(CValidationState& state, const CChainParams& chainparams, const CBlock& block, CBlockIndex* pindexPrev, bool fCheckMerkleRoot)
{
    AssertLockHeld(cs_main);
    assert(pindexPrev == chainActive.Tip());

    CCoinsViewCache viewNew(pcoinsTip);
    CBlockIndex indexDummy(block);
    indexDummy.pprev = pindexPrev;
    indexDummy.nHeight = pindexPrev->nHeight + 1;

    // JoinSplit proofs and Orchard authorizations are verified in ConnectBlock
    auto verifier = ProofVerifier::Disabled();
    auto orchardAuth = orchard::AuthValidator::Disabled();

    // NOTE: CheckBlockHeader is called by CheckBlock
    if (!ContextualCheckBlockHeader(block, state, chainparams, pindexPrev))
        return false;
    // The following may be duplicative of the `CheckBlock` call within `ConnectBlock`
    if (!CheckBlock(block, state, chainparams, verifier, orchardAuth, false, fCheckMerkleRoot, true))
        return false;
    if (!ContextualCheckBlock(block, state, chainparams, pindexPrev, true))
        return false;
    if (!ConnectBlock(block, state, &indexDummy, viewNew, chainparams, true, fCheckMerkleRoot))
        return false;
    assert(state.IsValid());

    return true;
}

/**
 * BLOCK PRUNING CODE
 */

/* Calculate the amount of disk space the block & undo files currently use */
uint64_t CalculateCurrentUsage()
{
    uint64_t retval = 0;
    for (const CBlockFileInfo &file : vinfoBlockFile) {
        retval += file.nSize + file.nUndoSize;
    }
    return retval;
}

/* Prune a block file (modify associated database entries)*/
void PruneOneBlockFile(const int fileNumber)
{
    for (BlockMap::iterator it = mapBlockIndex.begin(); it != mapBlockIndex.end(); ++it) {
        CBlockIndex* pindex = it->second;
        if (pindex->nFile == fileNumber) {
            pindex->nStatus &= ~BLOCK_HAVE_DATA;
            pindex->nStatus &= ~BLOCK_HAVE_UNDO;
            pindex->nFile = 0;
            pindex->nDataPos = 0;
            pindex->nUndoPos = 0;
            setDirtyBlockIndex.insert(pindex);

            // Prune from mapBlocksUnlinked -- any block we prune would have
            // to be downloaded again in order to consider its chain, at which
            // point it would be considered as a candidate for
            // mapBlocksUnlinked or setBlockIndexCandidates.
            std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> range = mapBlocksUnlinked.equal_range(pindex->pprev);
            while (range.first != range.second) {
                if (range.first->second == pindex) {
                    range.first = mapBlocksUnlinked.erase(range.first);
                } else {
                    ++range.first;
                }
            }
        }
    }

    vinfoBlockFile[fileNumber].SetNull();
    setDirtyFileInfo.insert(fileNumber);
}


void UnlinkPrunedFiles(std::set<int>& setFilesToPrune)
{
    for (set<int>::iterator it = setFilesToPrune.begin(); it != setFilesToPrune.end(); ++it) {
        CDiskBlockPos pos(*it, 0);
        fs::remove(GetBlockPosFilename(pos, "blk"));
        fs::remove(GetBlockPosFilename(pos, "rev"));
        LogPrintf("Prune: %s deleted blk/rev (%05u)\n", __func__, *it);
    }
}

/* Calculate the block/rev files that should be deleted to remain under target*/
void FindFilesToPrune(std::set<int>& setFilesToPrune, uint64_t nPruneAfterHeight)
{
    LOCK2(cs_main, cs_LastBlockFile);
    if (chainActive.Tip() == NULL || nPruneTarget == 0) {
        return;
    }
    if (chainActive.Tip()->nHeight <= nPruneAfterHeight) {
        return;
    }

    unsigned int nLastBlockWeCanPrune = chainActive.Tip()->nHeight - MIN_BLOCKS_TO_KEEP;
    uint64_t nCurrentUsage = CalculateCurrentUsage();
    // We don't check to prune until after we've allocated new space for files
    // So we should leave a buffer under our target to account for another allocation
    // before the next pruning.
    uint64_t nBuffer = BLOCKFILE_CHUNK_SIZE + UNDOFILE_CHUNK_SIZE;
    uint64_t nBytesToPrune;
    int count=0;

    if (nCurrentUsage + nBuffer >= nPruneTarget) {
        for (int fileNumber = 0; fileNumber < nLastBlockFile; fileNumber++) {
            nBytesToPrune = vinfoBlockFile[fileNumber].nSize + vinfoBlockFile[fileNumber].nUndoSize;

            if (vinfoBlockFile[fileNumber].nSize == 0)
                continue;

            if (nCurrentUsage + nBuffer < nPruneTarget)  // are we below our target?
                break;

            // don't prune files that could have a block within MIN_BLOCKS_TO_KEEP of the main chain's tip but keep scanning
            if (vinfoBlockFile[fileNumber].nHeightLast > nLastBlockWeCanPrune)
                continue;

            PruneOneBlockFile(fileNumber);
            // Queue up the files for removal
            setFilesToPrune.insert(fileNumber);
            nCurrentUsage -= nBytesToPrune;
            count++;
        }
    }

    LogPrint("prune", "Prune: target=%dMiB actual=%dMiB diff=%dMiB max_prune_height=%d removed %d blk/rev pairs\n",
           nPruneTarget/1024/1024, nCurrentUsage/1024/1024,
           ((int64_t)nPruneTarget - (int64_t)nCurrentUsage)/1024/1024,
           nLastBlockWeCanPrune, count);
}

bool CheckDiskSpace(uint64_t nAdditionalBytes)
{
    uint64_t nFreeBytesAvailable = fs::space(GetDataDir()).available;

    // Check for nMinDiskSpace bytes (currently 50MB)
    if (nFreeBytesAvailable < nMinDiskSpace + nAdditionalBytes)
        return AbortNode("Disk space is low!", _("Error: Disk space is low!"));

    return true;
}

FILE* OpenDiskFile(const CDiskBlockPos &pos, const char *prefix, bool fReadOnly)
{
    if (pos.IsNull())
        return NULL;
    fs::path path = GetBlockPosFilename(pos, prefix);
    fs::create_directories(path.parent_path());
    FILE* file = fsbridge::fopen(path, "rb+");
    if (!file && !fReadOnly)
        file = fsbridge::fopen(path, "wb+");
    if (!file) {
        LogPrintf("Unable to open file %s\n", path.string());
        return NULL;
    }
    if (pos.nPos) {
        if (fseek(file, pos.nPos, SEEK_SET)) {
            LogPrintf("Unable to seek to position %u of %s\n", pos.nPos, path.string());
            fclose(file);
            return NULL;
        }
    }
    return file;
}

FILE* OpenBlockFile(const CDiskBlockPos &pos, bool fReadOnly) {
    return OpenDiskFile(pos, "blk", fReadOnly);
}

FILE* OpenUndoFile(const CDiskBlockPos &pos, bool fReadOnly) {
    return OpenDiskFile(pos, "rev", fReadOnly);
}

fs::path GetBlockPosFilename(const CDiskBlockPos &pos, const char *prefix)
{
    return GetDataDir() / "blocks" / strprintf("%s%05u.dat", prefix, pos.nFile);
}

CBlockIndex * InsertBlockIndex(uint256 hash)
{
    if (hash.IsNull())
        return NULL;

    // Return existing
    BlockMap::iterator mi = mapBlockIndex.find(hash);
    if (mi != mapBlockIndex.end())
        return (*mi).second;

    // Create new
    CBlockIndex* pindexNew = new CBlockIndex();
    if (!pindexNew)
        throw runtime_error("LoadBlockIndex(): new CBlockIndex failed");
    mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);

    return pindexNew;
}

bool static LoadBlockIndexDB(const CChainParams& chainparams)
{
    if (!pblocktree->LoadBlockIndexGuts(InsertBlockIndex, chainparams))
        return false;

    // Calculate nChainWork
    vector<pair<int, CBlockIndex*> > vSortedByHeight;
    vSortedByHeight.reserve(mapBlockIndex.size());
    for (const std::pair<uint256, CBlockIndex*>& item : mapBlockIndex)
    {
        CBlockIndex* pindex = item.second;
        vSortedByHeight.push_back(make_pair(pindex->nHeight, pindex));
    }
    sort(vSortedByHeight.begin(), vSortedByHeight.end());
    for (const std::pair<int, CBlockIndex*>& item : vSortedByHeight)
    {
        CBlockIndex* pindex = item.second;
        pindex->nChainWork = (pindex->pprev ? pindex->pprev->nChainWork : 0) + GetBlockProof(*pindex);
        // We can link the chain of blocks for which we've received transactions at some point.
        // Pruned nodes may have deleted the block.
        if (pindex->nTx > 0) {
            if (pindex->pprev) {
                if (pindex->pprev->nChainTx) {
                    pindex->nChainTx = pindex->pprev->nChainTx + pindex->nTx;
                    if (pindex->pprev->nChainSproutValue && pindex->nSproutValue) {
                        pindex->nChainSproutValue = *pindex->pprev->nChainSproutValue + *pindex->nSproutValue;
                    } else {
                        pindex->nChainSproutValue = std::nullopt;
                    }
                    if (pindex->pprev->nChainSaplingValue) {
                        pindex->nChainSaplingValue = *pindex->pprev->nChainSaplingValue + pindex->nSaplingValue;
                    } else {
                        pindex->nChainSaplingValue = std::nullopt;
                    }
                    if (pindex->pprev->nChainOrchardValue) {
                        pindex->nChainOrchardValue = *pindex->pprev->nChainOrchardValue + pindex->nOrchardValue;
                    } else {
                        pindex->nChainOrchardValue = std::nullopt;
                    }
                } else {
                    pindex->nChainTx = 0;
                    pindex->nChainSproutValue = std::nullopt;
                    pindex->nChainSaplingValue = std::nullopt;
                    pindex->nChainOrchardValue = std::nullopt;
                    mapBlocksUnlinked.insert(std::make_pair(pindex->pprev, pindex));
                }
            } else {
                pindex->nChainTx = pindex->nTx;
                pindex->nChainSproutValue = pindex->nSproutValue;
                pindex->nChainSaplingValue = pindex->nSaplingValue;
                pindex->nChainOrchardValue = pindex->nOrchardValue;
            }

            // Fall back to hardcoded Sprout value pool balance
            FallbackSproutValuePoolBalance(pindex, chainparams);

            // If developer option -developersetpoolsizezero has been enabled,
            // override and set the in-memory size of shielded pools to zero.  An unshielding transaction
            // can then be used to trigger and test the handling of turnstile violations.
            if (fExperimentalDeveloperSetPoolSizeZero) {
                pindex->nChainSproutValue = 0;
                pindex->nChainSaplingValue = 0;
                pindex->nChainOrchardValue = 0;
            }
        }
        // Construct in-memory chain of branch IDs.
        // Relies on invariant: a block that does not activate a network upgrade
        // will always be valid under the same consensus rules as its parent.
        // Genesis block has a branch ID of zero by definition, but has no
        // validity status because it is side-loaded into a fresh chain.
        // Activation blocks will have branch IDs set (read from disk).
        if (pindex->pprev) {
            if (pindex->IsValid(BLOCK_VALID_CONSENSUS) && !pindex->nCachedBranchId) {
                pindex->nCachedBranchId = pindex->pprev->nCachedBranchId;
            }
        } else {
            pindex->nCachedBranchId = SPROUT_BRANCH_ID;
        }
        if (pindex->IsValid(BLOCK_VALID_TRANSACTIONS) && (pindex->nChainTx || pindex->pprev == NULL))
            setBlockIndexCandidates.insert(pindex);
        if (pindex->nStatus & BLOCK_FAILED_MASK && (!pindexBestInvalid || pindex->nChainWork > pindexBestInvalid->nChainWork))
            pindexBestInvalid = pindex;
        if (pindex->pprev)
            pindex->BuildSkip();
        if (pindex->IsValid(BLOCK_VALID_TREE) && (pindexBestHeader == NULL || CBlockIndexWorkComparator()(pindexBestHeader, pindex)))
            pindexBestHeader = pindex;
    }

    // Load block file info
    pblocktree->ReadLastBlockFile(nLastBlockFile);
    vinfoBlockFile.resize(nLastBlockFile + 1);
    LogPrintf("%s: last block file = %i\n", __func__, nLastBlockFile);
    for (int nFile = 0; nFile <= nLastBlockFile; nFile++) {
        pblocktree->ReadBlockFileInfo(nFile, vinfoBlockFile[nFile]);
    }
    LogPrintf("%s: last block file info: %s\n", __func__, vinfoBlockFile[nLastBlockFile].ToString());
    for (int nFile = nLastBlockFile + 1; true; nFile++) {
        CBlockFileInfo info;
        if (pblocktree->ReadBlockFileInfo(nFile, info)) {
            vinfoBlockFile.push_back(info);
        } else {
            break;
        }
    }

    // Check presence of blk files
    LogPrintf("Checking all blk files are present...\n");
    set<int> setBlkDataFiles;
    for (const std::pair<uint256, CBlockIndex*>& item : mapBlockIndex)
    {
        CBlockIndex* pindex = item.second;
        if (pindex->nStatus & BLOCK_HAVE_DATA) {
            setBlkDataFiles.insert(pindex->nFile);
        }
    }
    for (std::set<int>::iterator it = setBlkDataFiles.begin(); it != setBlkDataFiles.end(); it++)
    {
        CDiskBlockPos pos(*it, 0);
        if (CAutoFile(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION).IsNull()) {
            return false;
        }
    }

    // Check whether we have ever pruned block & undo files
    pblocktree->ReadFlag("prunedblockfiles", fHavePruned);
    if (fHavePruned)
        LogPrintf("LoadBlockIndexDB(): Block files have previously been pruned\n");

    // Check whether we need to continue reindexing
    bool fReindexing = false;
    pblocktree->ReadReindexing(fReindexing);
    if(fReindexing) fReindex = true;

    // Check whether we have a transaction index
    pblocktree->ReadFlag("txindex", fTxIndex);
    LogPrintf("%s: transaction index %s\n", __func__, fTxIndex ? "enabled" : "disabled");

    // insightexplorer and lightwalletd
    // Check whether block explorer features are enabled
    bool fInsightExplorer = false;
    bool fLightWalletd = false;
    pblocktree->ReadFlag("insightexplorer", fInsightExplorer);
    pblocktree->ReadFlag("lightwalletd", fLightWalletd);
    LogPrintf("%s: insight explorer %s\n", __func__, fInsightExplorer ? "enabled" : "disabled");
    LogPrintf("%s: light wallet daemon %s\n", __func__, fLightWalletd ? "enabled" : "disabled");
    if (fInsightExplorer) {
        fAddressIndex = true;
        fSpentIndex = true;
        fTimestampIndex = true;
    }
    else if (fLightWalletd) {
        fAddressIndex = true;
    }

    // Fill in-memory data
    for (const std::pair<uint256, CBlockIndex*>& item : mapBlockIndex)
    {
        CBlockIndex* pindex = item.second;
        // - This relationship will always be true even if pprev has multiple
        //   children, because hashSproutAnchor is technically a property of pprev,
        //   not its children.
        // - This will miss chain tips; we handle the best tip below, and other
        //   tips will be handled by ConnectTip during a re-org.
        if (pindex->pprev) {
            pindex->pprev->hashFinalSproutRoot = pindex->hashSproutAnchor;
        }
    }

    // Load pointer to end of best chain
    BlockMap::iterator it = mapBlockIndex.find(pcoinsTip->GetBestBlock());
    if (it == mapBlockIndex.end())
        return true;
    chainActive.SetTip(it->second);
    // Set hashFinalSproutRoot for the end of best chain
    it->second->hashFinalSproutRoot = pcoinsTip->GetBestAnchor(SPROUT);

    PruneBlockIndexCandidates();

    LogPrintf("%s: hashBestChain=%s height=%d date=%s progress=%f\n", __func__,
        chainActive.Tip()->GetBlockHash().ToString(), chainActive.Height(),
        DateTimeStrFormat("%Y-%m-%d %H:%M:%S", chainActive.Tip()->GetBlockTime()),
        Checkpoints::GuessVerificationProgress(chainparams.Checkpoints(), chainActive.Tip()));

    EnforceNodeDeprecation(chainActive.Height(), true);

    return true;
}

CVerifyDB::CVerifyDB()
{
    uiInterface.ShowProgress(_("Verifying blocks..."), 0);
}

CVerifyDB::~CVerifyDB()
{
    uiInterface.ShowProgress("", 100);
}

bool CVerifyDB::VerifyDB(const CChainParams& chainparams, CCoinsView *coinsview, int nCheckLevel, int nCheckDepth)
{
    LOCK(cs_main);
    if (chainActive.Tip() == NULL || chainActive.Tip()->pprev == NULL)
        return true;

    // Verify blocks in the best chain
    if (nCheckDepth <= 0)
        nCheckDepth = 1000000000; // suffices until the year 19000
    if (nCheckDepth > chainActive.Height())
        nCheckDepth = chainActive.Height();
    nCheckLevel = std::max(0, std::min(4, nCheckLevel));
    LogPrintf("Verifying last %i blocks at level %i\n", nCheckDepth, nCheckLevel);
    CCoinsViewCache coins(coinsview);
    CBlockIndex* pindexState = chainActive.Tip();
    CBlockIndex* pindexFailure = NULL;
    int nGoodTransactions = 0;
    CValidationState state;

    // Flags used to permit skipping checks for efficiency
    auto verifier = ProofVerifier::Disabled(); // No need to verify JoinSplits twice
    bool fCheckTransactions = true;
    // We may as well check Orchard authorizations if we are checking
    // transactions, since we can batch-validate them.
    auto orchardAuth = orchard::AuthValidator::Batch();

    for (CBlockIndex* pindex = chainActive.Tip(); pindex && pindex->pprev; pindex = pindex->pprev)
    {
        boost::this_thread::interruption_point();
        uiInterface.ShowProgress(_("Verifying blocks..."), std::max(1, std::min(99, (int)(((double)(chainActive.Height() - pindex->nHeight)) / (double)nCheckDepth * (nCheckLevel >= 4 ? 50 : 100)))));
        if (pindex->nHeight < chainActive.Height()-nCheckDepth)
            break;

        CBlock block;
        // check level 0: read from disk
        if (!ReadBlockFromDisk(block, pindex, chainparams.GetConsensus()))
            return error("VerifyDB(): *** ReadBlockFromDisk failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());

        // check level 1: verify block validity
        fCheckTransactions = ShouldCheckTransactions(chainparams, pindex);
        if (nCheckLevel >= 1 && !CheckBlock(block, state, chainparams, verifier, orchardAuth, true, true, fCheckTransactions))
            return error("VerifyDB(): *** found bad block at %d, hash=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString());

        // check level 2: verify undo validity
        if (nCheckLevel >= 2 && pindex) {
            CBlockUndo undo;
            CDiskBlockPos pos = pindex->GetUndoPos();
            if (!pos.IsNull()) {
                if (!UndoReadFromDisk(undo, pos, pindex->pprev->GetBlockHash()))
                    return error("VerifyDB(): *** found bad undo data at %d, hash=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
            }
        }

        // check level 3: check for inconsistencies during memory-only disconnect of tip blocks
        if (nCheckLevel >= 3 && pindex == pindexState && (coins.DynamicMemoryUsage() + pcoinsTip->DynamicMemoryUsage()) <= nCoinCacheUsage) {
            // insightexplorer: do not update indices (false)
            DisconnectResult res = DisconnectBlock(block, state, pindex, coins, chainparams, false);
            if (res == DISCONNECT_FAILED) {
                return error("VerifyDB(): *** irrecoverable inconsistency in block data at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
            }
            pindexState = pindex->pprev;
            if (res == DISCONNECT_UNCLEAN) {
                nGoodTransactions = 0;
                pindexFailure = pindex;
            } else {
                nGoodTransactions += block.vtx.size();
            }
        }

        if (ShutdownRequested())
            return true;
    }
    if (pindexFailure)
        return error("VerifyDB(): *** coin database inconsistencies found (last %i blocks, %i good transactions before that)\n", chainActive.Height() - pindexFailure->nHeight + 1, nGoodTransactions);

    // check level 4: try reconnecting blocks
    if (nCheckLevel >= 4) {
        CBlockIndex *pindex = pindexState;
        while (pindex != chainActive.Tip()) {
            boost::this_thread::interruption_point();
            uiInterface.ShowProgress(_("Verifying blocks..."), std::max(1, std::min(99, 100 - (int)(((double)(chainActive.Height() - pindex->nHeight)) / (double)nCheckDepth * 50))));
            pindex = chainActive.Next(pindex);
            CBlock block;
            if (!ReadBlockFromDisk(block, pindex, chainparams.GetConsensus()))
                return error("VerifyDB(): *** ReadBlockFromDisk failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
            if (!ConnectBlock(block, state, pindex, coins, chainparams))
                return error("VerifyDB(): *** found unconnectable block at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
        }
    }

    LogPrintf("No coin database inconsistencies in last %i blocks (%i transactions)\n", chainActive.Height() - pindexState->nHeight, nGoodTransactions);

    return true;
}

bool RewindBlockIndex(const CChainParams& chainparams, bool& clearWitnessCaches)
{
    LOCK(cs_main);

    // RewindBlockIndex is called after LoadBlockIndex, so at this point every block
    // index will have nCachedBranchId set based on the values previously persisted
    // to disk. By definition, a set nCachedBranchId means that the block was
    // fully-validated under the corresponding consensus rules. Thus we can quickly
    // identify whether the current active chain matches our expected sequence of
    // consensus rule changes, with two checks:
    //
    // - BLOCK_ACTIVATES_UPGRADE is set only on blocks that activate upgrades.
    // - nCachedBranchId for each block matches what we expect.
    auto sufficientlyValidated = [&chainparams](const CBlockIndex* pindex) {
        const Consensus::Params& consensus = chainparams.GetConsensus();
        bool fFlagSet = pindex->nStatus & BLOCK_ACTIVATES_UPGRADE;
        bool fFlagExpected = IsActivationHeightForAnyUpgrade(pindex->nHeight, consensus);
        return fFlagSet == fFlagExpected &&
            pindex->nCachedBranchId &&
            *pindex->nCachedBranchId == CurrentEpochBranchId(pindex->nHeight, consensus);
    };

    int lastValidHeight = 0;
    while (lastValidHeight < chainActive.Height()) {
        if (sufficientlyValidated(chainActive[lastValidHeight + 1])) {
            lastValidHeight++;
        } else {
            break;
        }
    }

    // lastValidHeight is now the height of the last valid block below the active chain height
    auto rewindLength = chainActive.Height() - lastValidHeight;
    clearWitnessCaches = false;

    if (rewindLength > 0) {
        LogPrintf("*** Last validated block at height %d, active height is %d; rewind length %d\n", lastValidHeight, chainActive.Height(), rewindLength);

        auto nHeight = lastValidHeight + 1;
        const uint256 *phashFirstInsufValidated = chainActive[nHeight]->phashBlock;
        auto networkID = chainparams.NetworkIDString();

        // This is true when we intend to do a long rewind.
        bool intendedRewind =
            (networkID == "test" && nHeight == 252500 && *phashFirstInsufValidated ==
             uint256S("0018bd16a9c6f15795a754c498d2b2083ab78f14dae44a66a8d0e90ba8464d9c")) ||
            (networkID == "test" && nHeight == 584000 && *phashFirstInsufValidated ==
             uint256S("002e1d6daf4ab7b296e7df839dc1bee9d615583bb4bc34b1926ce78307532852")) ||
            (networkID == "test" && nHeight == 903800 && *phashFirstInsufValidated ==
             uint256S("0044f3c696a242220ed608c70beba5aa804f1cfb8a20e32186727d1bec5dfa1e"));

        clearWitnessCaches = (rewindLength > MAX_REORG_LENGTH && intendedRewind);
        if (clearWitnessCaches) {
            auto msg = strprintf(_(
                "An intended block chain rewind has been detected: network %s, hash %s, height %d"
                ), networkID, phashFirstInsufValidated->GetHex(), nHeight);
            LogPrintf("*** %s\n", msg);
        }

        if (rewindLength > MAX_REORG_LENGTH && !intendedRewind) {
            auto pindexOldTip = chainActive.Tip();
            auto pindexRewind = chainActive[lastValidHeight];
            auto msg = strprintf(_(
                "A block chain rewind has been detected that would roll back %d blocks! "
                "This is larger than the maximum of %d blocks, and so the node is shutting down for your safety."
                ), rewindLength, MAX_REORG_LENGTH) + "\n\n" +
                _("Rewind details") + ":\n" +
                "- " + strprintf(_("Current tip:   %s, height %d"),
                    pindexOldTip->phashBlock->GetHex(), pindexOldTip->nHeight) + "\n" +
                "- " + strprintf(_("Rewinding to:  %s, height %d"),
                    pindexRewind->phashBlock->GetHex(), pindexRewind->nHeight) + "\n\n" +
                _("Please help, human!");
            LogPrintf("*** %s\n", msg);
            uiInterface.ThreadSafeMessageBox(msg, "", CClientUIInterface::MSG_ERROR);
            StartShutdown();
            return false;
        }
    }

    CValidationState state;
    CBlockIndex* pindex = chainActive.Tip();
    while (chainActive.Height() > lastValidHeight) {
        if (fPruneMode && !(chainActive.Tip()->nStatus & BLOCK_HAVE_DATA)) {
            // If pruning, don't try rewinding past the HAVE_DATA point;
            // since older blocks can't be served anyway, there's
            // no need to walk further, and trying to DisconnectTip()
            // will fail (and require a needless reindex/redownload
            // of the blockchain).
            break;
        }
        if (!DisconnectTip(state, chainparams, true)) {
            return error("RewindBlockIndex: unable to disconnect block at height %i", pindex->nHeight);
        }
        // Occasionally flush state to disk.
        if (!FlushStateToDisk(chainparams, state, FLUSH_STATE_PERIODIC))
            return false;
    }

    // Collect blocks to be removed (blocks in mapBlockIndex must be at least BLOCK_VALID_TREE).
    // We do this after actual disconnecting, otherwise we'll end up writing the lack of data
    // to disk before writing the chainstate, resulting in a failure to continue if interrupted.
    std::vector<const CBlockIndex*> vBlocks;
    for (BlockMap::iterator it = mapBlockIndex.begin(); it != mapBlockIndex.end(); it++) {
        CBlockIndex* pindexIter = it->second;

        // Note: If we encounter an insufficiently validated block that
        // is on chainActive, it must be because we are a pruning node, and
        // this block or some successor doesn't HAVE_DATA, so we were unable to
        // rewind all the way.  Blocks remaining on chainActive at this point
        // must not have their validity reduced.
        if (!sufficientlyValidated(pindexIter) && !chainActive.Contains(pindexIter)) {
            // Add to the list of blocks to remove
            vBlocks.push_back(pindexIter);
            if (pindexIter == pindexBestInvalid) {
                // Reset invalid block marker if it was pointing to this block
                pindexBestInvalid = NULL;
            }
            // Update indices
            setBlockIndexCandidates.erase(pindexIter);
            auto ret = mapBlocksUnlinked.equal_range(pindexIter->pprev);
            while (ret.first != ret.second) {
                if (ret.first->second == pindexIter) {
                    ret.first = mapBlocksUnlinked.erase(ret.first);
                } else {
                    ++ret.first;
                }
            }
        } else if (pindexIter->IsValid(BLOCK_VALID_TRANSACTIONS) && pindexIter->nChainTx) {
            setBlockIndexCandidates.insert(pindexIter);
        }
    }

    // Set pindexBestHeader to the current chain tip
    // (since we are about to delete the block it is pointing to)
    pindexBestHeader = chainActive.Tip();

    // Erase block indices on-disk
    if (!pblocktree->EraseBatchSync(vBlocks)) {
        return AbortNode(state, "Failed to erase from block index database");
    }

    // Erase block indices in-memory
    for (auto pindex : vBlocks) {
        auto ret = mapBlockIndex.find(*pindex->phashBlock);
        if (ret != mapBlockIndex.end()) {
            mapBlockIndex.erase(ret);
            delete pindex;
        }
    }

    PruneBlockIndexCandidates();

    // Ensure that pindexBestHeader points to the block index entry with the most work;
    // setBlockIndexCandidates entries are sorted by work, highest at the end.
    {
        std::set<CBlockIndex*, CBlockIndexWorkComparator>::reverse_iterator it = setBlockIndexCandidates.rbegin();
        assert(it != setBlockIndexCandidates.rend());
        pindexBestHeader = *it;
    }

    CheckBlockIndex(chainparams.GetConsensus());

    if (!FlushStateToDisk(chainparams, state, FLUSH_STATE_ALWAYS)) {
        return false;
    }

    return true;
}

void UnloadBlockIndex()
{
    LOCK(cs_main);
    setBlockIndexCandidates.clear();
    chainActive.SetTip(NULL);
    pindexBestInvalid = NULL;
    pindexBestHeader = NULL;
    mempool.clear();
    mapOrphanTransactions.clear();
    mapOrphanTransactionsByPrev.clear();
    nSyncStarted = 0;
    mapBlocksUnlinked.clear();
    vinfoBlockFile.clear();
    nLastBlockFile = 0;
    nBlockSequenceId = 1;
    mapBlockSource.clear();
    mapBlocksInFlight.clear();
    nQueuedValidatedHeaders = 0;
    nPreferredDownload = 0;
    setDirtyBlockIndex.clear();
    setDirtyFileInfo.clear();
    mapNodeState.clear();
    recentRejects.reset(NULL);

    for (BlockMap::value_type& entry : mapBlockIndex) {
        delete entry.second;
    }
    mapBlockIndex.clear();
    fHavePruned = false;
}

bool LoadBlockIndex()
{
    // Load block index from databases
    if (!fReindex && !LoadBlockIndexDB(Params()))
        return false;
    return true;
}

bool InitBlockIndex(const CChainParams& chainparams)
{
    LOCK(cs_main);

    // Initialize global variables that cannot be constructed at startup.
    recentRejects.reset(new CRollingBloomFilter(120000, 0.000001));

    // Check whether we're already initialized
    if (chainActive.Genesis() != NULL)
        return true;

    // Use the provided setting for -txindex in the new database
    fTxIndex = GetBoolArg("-txindex", DEFAULT_TXINDEX);
    pblocktree->WriteFlag("txindex", fTxIndex);

    // Use the provided setting for -insightexplorer or -lightwalletd in the new database
    pblocktree->WriteFlag("insightexplorer", fExperimentalInsightExplorer);
    pblocktree->WriteFlag("lightwalletd", fExperimentalLightWalletd);
    if (fExperimentalInsightExplorer) {
        fAddressIndex = true;
        fSpentIndex = true;
        fTimestampIndex = true;
    }
    else if (fExperimentalLightWalletd) {
        fAddressIndex = true;
    }

    LogPrintf("Initializing databases...\n");

    // Only add the genesis block if not reindexing (in which case we reuse the one already on disk)
    if (!fReindex) {
        try {
            CBlock &block = const_cast<CBlock&>(chainparams.GenesisBlock());
            // Start new block file
            unsigned int nBlockSize = ::GetSerializeSize(block, SER_DISK, CLIENT_VERSION);
            CDiskBlockPos blockPos;
            CValidationState state;
            if (!FindBlockPos(state, blockPos, nBlockSize+8, 0, block.GetBlockTime()))
                return error("LoadBlockIndex(): FindBlockPos failed");
            if (!WriteBlockToDisk(block, blockPos, chainparams.MessageStart()))
                return error("LoadBlockIndex(): writing genesis block to disk failed");
            CBlockIndex *pindex = AddToBlockIndex(block, chainparams.GetConsensus());
            if (!ReceivedBlockTransactions(block, state, chainparams, pindex, blockPos))
                return error("LoadBlockIndex(): genesis block not accepted");
            if (!ActivateBestChain(state, chainparams, &block))
                return error("LoadBlockIndex(): genesis block cannot be activated");
            // Force a chainstate write so that when we VerifyDB in a moment, it doesn't check stale data
            return FlushStateToDisk(chainparams, state, FLUSH_STATE_ALWAYS);
        } catch (const std::runtime_error& e) {
            return error("LoadBlockIndex(): failed to initialize block database: %s", e.what());
        }
    }

    return true;
}

bool LoadExternalBlockFile(const CChainParams& chainparams, FILE* fileIn, CDiskBlockPos *dbp)
{
    // Map of disk positions for blocks with unknown parent (only used for reindex)
    static std::multimap<uint256, CDiskBlockPos> mapBlocksUnknownParent;
    int64_t nStart = GetTimeMillis();

    int nLoaded = 0;
    try {
        // This takes over fileIn and calls fclose() on it in the CBufferedFile destructor
        CBufferedFile blkdat(fileIn, 2*MAX_BLOCK_SIZE, MAX_BLOCK_SIZE+8, SER_DISK, CLIENT_VERSION);
        uint64_t nRewind = blkdat.GetPos();
        size_t initialSize = nSizeReindexed;
        while (!blkdat.eof()) {
            boost::this_thread::interruption_point();

            if (fReindex)
               nSizeReindexed = initialSize + nRewind;

            blkdat.SetPos(nRewind);
            nRewind++; // start one byte further next time, in case of failure
            blkdat.SetLimit(); // remove former limit
            unsigned int nSize = 0;
            try {
                // locate a header
                unsigned char buf[MESSAGE_START_SIZE];
                blkdat.FindByte(chainparams.MessageStart()[0]);
                nRewind = blkdat.GetPos()+1;
                blkdat >> FLATDATA(buf);
                if (memcmp(buf, chainparams.MessageStart(), MESSAGE_START_SIZE))
                    continue;
                // read size
                blkdat >> nSize;
                if (nSize < 80 || nSize > MAX_BLOCK_SIZE)
                    continue;
            } catch (const std::exception&) {
                // no valid block header found; don't complain
                break;
            }
            try {
                // read block
                uint64_t nBlockPos = blkdat.GetPos();
                if (dbp)
                    dbp->nPos = nBlockPos;
                blkdat.SetLimit(nBlockPos + nSize);
                blkdat.SetPos(nBlockPos);
                CBlock block;
                blkdat >> block;
                nRewind = blkdat.GetPos();

                // detect out of order blocks, and store them for later
                uint256 hash = block.GetHash();
                if (hash != chainparams.GetConsensus().hashGenesisBlock && mapBlockIndex.find(block.hashPrevBlock) == mapBlockIndex.end()) {
                    LogPrint("reindex", "%s: Out of order block %s, parent %s not known\n", __func__, hash.ToString(),
                            block.hashPrevBlock.ToString());
                    if (dbp)
                        mapBlocksUnknownParent.insert(std::make_pair(block.hashPrevBlock, *dbp));
                    continue;
                }

                // process in case the block isn't known yet
                if (mapBlockIndex.count(hash) == 0 || (mapBlockIndex[hash]->nStatus & BLOCK_HAVE_DATA) == 0) {
                    LOCK(cs_main);
                    CValidationState state;
                    if (AcceptBlock(block, state, chainparams, NULL, true, dbp))
                        nLoaded++;
                    if (state.IsError())
                        break;
                } else if (hash != chainparams.GetConsensus().hashGenesisBlock && mapBlockIndex[hash]->nHeight % 1000 == 0) {
                    LogPrint("reindex", "Block Import: already had block %s at height %d\n", hash.ToString(), mapBlockIndex[hash]->nHeight);
                }

                // Activate the genesis block so normal node progress can continue
                if (hash == chainparams.GetConsensus().hashGenesisBlock) {
                    CValidationState state;
                    if (!ActivateBestChain(state, chainparams)) {
                        break;
                    }
                }

                NotifyHeaderTip(chainparams.GetConsensus());

                // Recursively process earlier encountered successors of this block
                deque<uint256> queue;
                queue.push_back(hash);
                while (!queue.empty()) {
                    uint256 head = queue.front();
                    queue.pop_front();
                    std::pair<std::multimap<uint256, CDiskBlockPos>::iterator, std::multimap<uint256, CDiskBlockPos>::iterator> range = mapBlocksUnknownParent.equal_range(head);
                    while (range.first != range.second) {
                        if (ReadBlockFromDisk(block, range.first->second, chainparams.GetConsensus()))
                        {
                            LogPrint("reindex", "%s: Processing out of order child %s of %s\n", __func__, block.GetHash().ToString(),
                                    head.ToString());
                            LOCK(cs_main);
                            CValidationState dummy;
                            if (AcceptBlock(block, dummy, chainparams, NULL, true, &(range.first->second)))
                            {
                                nLoaded++;
                                queue.push_back(block.GetHash());
                            }
                        }
                        range.first = mapBlocksUnknownParent.erase(range.first);
                        NotifyHeaderTip(chainparams.GetConsensus());
                    }
                }
            } catch (const std::exception& e) {
                LogPrintf("%s: Deserialize or I/O error - %s\n", __func__, e.what());
            }
        }
    } catch (const std::runtime_error& e) {
        AbortNode(std::string("System error: ") + e.what());
    }
    if (nLoaded > 0)
        LogPrintf("Loaded %i blocks from external file in %dms\n", nLoaded, GetTimeMillis() - nStart);
    return nLoaded > 0;
}

void static CheckBlockIndex(const Consensus::Params& consensusParams)
{
    if (!fCheckBlockIndex) {
        return;
    }

    LOCK(cs_main);

    // During a reindex, we read the genesis block and call CheckBlockIndex before ActivateBestChain,
    // so we have the genesis block in mapBlockIndex but no active chain.  (A few of the tests when
    // iterating the block tree require that chainActive has been initialized.)
    if (chainActive.Height() < 0) {
        assert(mapBlockIndex.size() <= 1);
        return;
    }

    // Build forward-pointing map of the entire block tree.
    std::multimap<CBlockIndex*,CBlockIndex*> forward;
    for (BlockMap::iterator it = mapBlockIndex.begin(); it != mapBlockIndex.end(); it++) {
        forward.insert(std::make_pair(it->second->pprev, it->second));
    }

    assert(forward.size() == mapBlockIndex.size());

    std::pair<std::multimap<CBlockIndex*,CBlockIndex*>::iterator,std::multimap<CBlockIndex*,CBlockIndex*>::iterator> rangeGenesis = forward.equal_range(NULL);
    CBlockIndex *pindex = rangeGenesis.first->second;
    rangeGenesis.first++;
    assert(rangeGenesis.first == rangeGenesis.second); // There is only one index entry with parent NULL.

    // Iterate over the entire block tree, using depth-first search.
    // Along the way, remember whether there are blocks on the path from genesis
    // block being explored which are the first to have certain properties.
    size_t nNodes = 0;
    int nHeight = 0;
    CBlockIndex* pindexFirstInvalid = NULL; // Oldest ancestor of pindex which is invalid.
    CBlockIndex* pindexFirstMissing = NULL; // Oldest ancestor of pindex which does not have BLOCK_HAVE_DATA.
    CBlockIndex* pindexFirstNeverProcessed = NULL; // Oldest ancestor of pindex for which nTx == 0.
    CBlockIndex* pindexFirstNotTreeValid = NULL; // Oldest ancestor of pindex which does not have BLOCK_VALID_TREE (regardless of being valid or not).
    CBlockIndex* pindexFirstNotTransactionsValid = NULL; // Oldest ancestor of pindex which does not have BLOCK_VALID_TRANSACTIONS (regardless of being valid or not).
    CBlockIndex* pindexFirstNotChainValid = NULL; // Oldest ancestor of pindex which does not have BLOCK_VALID_CHAIN (regardless of being valid or not).
    CBlockIndex* pindexFirstNotScriptsValid = NULL; // Oldest ancestor of pindex which does not have BLOCK_VALID_SCRIPTS (regardless of being valid or not).
    while (pindex != NULL) {
        nNodes++;
        if (pindexFirstInvalid == NULL && pindex->nStatus & BLOCK_FAILED_VALID) pindexFirstInvalid = pindex;
        if (pindexFirstMissing == NULL && !(pindex->nStatus & BLOCK_HAVE_DATA)) pindexFirstMissing = pindex;
        if (pindexFirstNeverProcessed == NULL && pindex->nTx == 0) pindexFirstNeverProcessed = pindex;
        if (pindex->pprev != NULL && pindexFirstNotTreeValid == NULL && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TREE) pindexFirstNotTreeValid = pindex;
        if (pindex->pprev != NULL && pindexFirstNotTransactionsValid == NULL && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TRANSACTIONS) pindexFirstNotTransactionsValid = pindex;
        if (pindex->pprev != NULL && pindexFirstNotChainValid == NULL && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_CHAIN) pindexFirstNotChainValid = pindex;
        if (pindex->pprev != NULL && pindexFirstNotScriptsValid == NULL && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_SCRIPTS) pindexFirstNotScriptsValid = pindex;

        // Begin: actual consistency checks.
        if (pindex->pprev == NULL) {
            // Genesis block checks.
            assert(pindex->GetBlockHash() == consensusParams.hashGenesisBlock); // Genesis block's hash must match.
            assert(pindex == chainActive.Genesis()); // The current active chain's genesis block must be this block.
        }
        if (pindex->nChainTx == 0) assert(pindex->nSequenceId == 0);  // nSequenceId can't be set for blocks that aren't linked
        // VALID_TRANSACTIONS is equivalent to nTx > 0 for all nodes (whether or not pruning has occurred).
        // HAVE_DATA is only equivalent to nTx > 0 (or VALID_TRANSACTIONS) if no pruning has occurred.
        if (!fHavePruned) {
            // If we've never pruned, then HAVE_DATA should be equivalent to nTx > 0
            assert(!(pindex->nStatus & BLOCK_HAVE_DATA) == (pindex->nTx == 0));
            assert(pindexFirstMissing == pindexFirstNeverProcessed);
        } else {
            // If we have pruned, then we can only say that HAVE_DATA implies nTx > 0
            if (pindex->nStatus & BLOCK_HAVE_DATA) assert(pindex->nTx > 0);
        }
        if (pindex->nStatus & BLOCK_HAVE_UNDO) assert(pindex->nStatus & BLOCK_HAVE_DATA);
        assert(((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TRANSACTIONS) == (pindex->nTx > 0)); // This is pruning-independent.
        // All parents having had data (at some point) is equivalent to all parents being VALID_TRANSACTIONS, which is equivalent to nChainTx being set.
        assert((pindexFirstNeverProcessed != NULL) == (pindex->nChainTx == 0)); // nChainTx != 0 is used to signal that all parent blocks have been processed (but may have been pruned).
        assert((pindexFirstNotTransactionsValid != NULL) == (pindex->nChainTx == 0));
        assert(pindex->nHeight == nHeight); // nHeight must be consistent.
        assert(pindex->pprev == NULL || pindex->nChainWork >= pindex->pprev->nChainWork); // For every block except the genesis block, the chainwork must be larger than the parent's.
        assert(nHeight < 2 || (pindex->pskip && (pindex->pskip->nHeight < nHeight))); // The pskip pointer must point back for all but the first 2 blocks.
        assert(pindexFirstNotTreeValid == NULL); // All mapBlockIndex entries must at least be TREE valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TREE) assert(pindexFirstNotTreeValid == NULL); // TREE valid implies all parents are TREE valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_CHAIN) assert(pindexFirstNotChainValid == NULL); // CHAIN valid implies all parents are CHAIN valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_SCRIPTS) assert(pindexFirstNotScriptsValid == NULL); // SCRIPTS valid implies all parents are SCRIPTS valid
        if (pindexFirstInvalid == NULL) {
            // Checks for not-invalid blocks.
            assert((pindex->nStatus & BLOCK_FAILED_MASK) == 0); // The failed mask cannot be set for blocks without invalid parents.
        }
        if (!CBlockIndexWorkComparator()(pindex, chainActive.Tip()) && pindexFirstNeverProcessed == NULL) {
            if (pindexFirstInvalid == NULL) {
                // If this block sorts at least as good as the current tip and
                // is valid and we have all data for its parents, it must be in
                // setBlockIndexCandidates.  chainActive.Tip() must also be there
                // even if some data has been pruned.
                if (pindexFirstMissing == NULL || pindex == chainActive.Tip()) {
                    assert(setBlockIndexCandidates.count(pindex));
                }
                // If some parent is missing, then it could be that this block was in
                // setBlockIndexCandidates but had to be removed because of the missing data.
                // In this case it must be in mapBlocksUnlinked -- see test below.
            }
        } else { // If this block sorts worse than the current tip or some ancestor's block has never been seen, it cannot be in setBlockIndexCandidates.
            assert(setBlockIndexCandidates.count(pindex) == 0);
        }
        // Check whether this block is in mapBlocksUnlinked.
        std::pair<std::multimap<CBlockIndex*,CBlockIndex*>::iterator,std::multimap<CBlockIndex*,CBlockIndex*>::iterator> rangeUnlinked = mapBlocksUnlinked.equal_range(pindex->pprev);
        bool foundInUnlinked = false;
        while (rangeUnlinked.first != rangeUnlinked.second) {
            assert(rangeUnlinked.first->first == pindex->pprev);
            if (rangeUnlinked.first->second == pindex) {
                foundInUnlinked = true;
                break;
            }
            rangeUnlinked.first++;
        }
        if (pindex->pprev && (pindex->nStatus & BLOCK_HAVE_DATA) && pindexFirstNeverProcessed != NULL && pindexFirstInvalid == NULL) {
            // If this block has block data available, some parent was never received, and has no invalid parents, it must be in mapBlocksUnlinked.
            assert(foundInUnlinked);
        }
        if (!(pindex->nStatus & BLOCK_HAVE_DATA)) assert(!foundInUnlinked); // Can't be in mapBlocksUnlinked if we don't HAVE_DATA
        if (pindexFirstMissing == NULL) assert(!foundInUnlinked); // We aren't missing data for any parent -- cannot be in mapBlocksUnlinked.
        if (pindex->pprev && (pindex->nStatus & BLOCK_HAVE_DATA) && pindexFirstNeverProcessed == NULL && pindexFirstMissing != NULL) {
            // We HAVE_DATA for this block, have received data for all parents at some point, but we're currently missing data for some parent.
            assert(fHavePruned); // We must have pruned.
            // This block may have entered mapBlocksUnlinked if:
            //  - it has a descendant that at some point had more work than the
            //    tip, and
            //  - we tried switching to that descendant but were missing
            //    data for some intermediate block between chainActive and the
            //    tip.
            // So if this block is itself better than chainActive.Tip() and it wasn't in
            // setBlockIndexCandidates, then it must be in mapBlocksUnlinked.
            if (!CBlockIndexWorkComparator()(pindex, chainActive.Tip()) && setBlockIndexCandidates.count(pindex) == 0) {
                if (pindexFirstInvalid == NULL) {
                    assert(foundInUnlinked);
                }
            }
        }
        // assert(pindex->GetBlockHash() == pindex->GetBlockHeader().GetHash()); // Perhaps too slow
        // End: actual consistency checks.

        // Try descending into the first subnode.
        std::pair<std::multimap<CBlockIndex*,CBlockIndex*>::iterator,std::multimap<CBlockIndex*,CBlockIndex*>::iterator> range = forward.equal_range(pindex);
        if (range.first != range.second) {
            // A subnode was found.
            pindex = range.first->second;
            nHeight++;
            continue;
        }
        // This is a leaf node.
        // Move upwards until we reach a node of which we have not yet visited the last child.
        while (pindex) {
            // We are going to either move to a parent or a sibling of pindex.
            // If pindex was the first with a certain property, unset the corresponding variable.
            if (pindex == pindexFirstInvalid) pindexFirstInvalid = NULL;
            if (pindex == pindexFirstMissing) pindexFirstMissing = NULL;
            if (pindex == pindexFirstNeverProcessed) pindexFirstNeverProcessed = NULL;
            if (pindex == pindexFirstNotTreeValid) pindexFirstNotTreeValid = NULL;
            if (pindex == pindexFirstNotTransactionsValid) pindexFirstNotTransactionsValid = NULL;
            if (pindex == pindexFirstNotChainValid) pindexFirstNotChainValid = NULL;
            if (pindex == pindexFirstNotScriptsValid) pindexFirstNotScriptsValid = NULL;
            // Find our parent.
            CBlockIndex* pindexPar = pindex->pprev;
            // Find which child we just visited.
            std::pair<std::multimap<CBlockIndex*,CBlockIndex*>::iterator,std::multimap<CBlockIndex*,CBlockIndex*>::iterator> rangePar = forward.equal_range(pindexPar);
            while (rangePar.first->second != pindex) {
                assert(rangePar.first != rangePar.second); // Our parent must have at least the node we're coming from as child.
                rangePar.first++;
            }
            // Proceed to the next one.
            rangePar.first++;
            if (rangePar.first != rangePar.second) {
                // Move to the sibling.
                pindex = rangePar.first->second;
                break;
            } else {
                // Move up further.
                pindex = pindexPar;
                nHeight--;
                continue;
            }
        }
    }

    // Check that we actually traversed the entire map.
    assert(nNodes == forward.size());
}



//////////////////////////////////////////////////////////////////////////////
//
// Messages
//


bool static AlreadyHave(const CInv& inv) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    switch (inv.type)
    {
    case MSG_TX:
        {
            assert(recentRejects);
            if (chainActive.Tip()->GetBlockHash() != hashRecentRejectsChainTip)
            {
                // If the chain tip has changed previously rejected transactions
                // might be now valid, e.g. due to a nLockTime'd tx becoming valid,
                // or a double-spend. Reset the rejects filter and give those
                // txs a second chance.
                hashRecentRejectsChainTip = chainActive.Tip()->GetBlockHash();
                recentRejects->reset();
            }

            return recentRejects->contains(inv.hash) ||
                   mempool.exists(inv.hash) ||
                   mapOrphanTransactions.count(inv.hash) ||
                   pcoinsTip->HaveCoins(inv.hash);
        }
    case MSG_BLOCK:
        return mapBlockIndex.count(inv.hash);
    }
    // Don't know what it is, just say we already got one
    return true;
}

void static ProcessGetData(CNode* pfrom, const Consensus::Params& consensusParams)
{
    int currentHeight = GetHeight();

    std::deque<CInv>::iterator it = pfrom->vRecvGetData.begin();

    vector<CInv> vNotFound;

    LOCK(cs_main);

    while (it != pfrom->vRecvGetData.end()) {
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize())
            break;

        const CInv &inv = *it;
        {
            boost::this_thread::interruption_point();
            it++;

            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK)
            {
                bool send = false;
                BlockMap::iterator mi = mapBlockIndex.find(inv.hash);
                if (mi != mapBlockIndex.end())
                {
                    if (chainActive.Contains(mi->second)) {
                        send = true;
                    } else {
                        static const int nOneMonth = 30 * 24 * 60 * 60;
                        // To prevent fingerprinting attacks, only send blocks outside of the active
                        // chain if they are valid, and no more than a month older (both in time, and in
                        // best equivalent proof of work) than the best header chain we know about.
                        send = mi->second->IsValid(BLOCK_VALID_SCRIPTS) && (pindexBestHeader != NULL) &&
                            (pindexBestHeader->GetBlockTime() - mi->second->GetBlockTime() < nOneMonth) &&
                            (GetBlockProofEquivalentTime(*pindexBestHeader, *mi->second, *pindexBestHeader, consensusParams) < nOneMonth);
                        if (!send) {
                            LogPrintf("%s: ignoring request from peer=%i for old block that isn't in the main chain\n", __func__, pfrom->GetId());
                        }
                    }
                }
                // disconnect node in case we have reached the outbound limit for serving historical blocks
                // never disconnect whitelisted nodes
                static const int nOneWeek = 7 * 24 * 60 * 60; // assume > 1 week = historical
                if (send && CNode::OutboundTargetReached(consensusParams.PoWTargetSpacing(currentHeight), true) && (
                        (
                            (pindexBestHeader != NULL) &&
                            (pindexBestHeader->GetBlockTime() - mi->second->GetBlockTime() > nOneWeek)
                        ) || inv.type == MSG_FILTERED_BLOCK
                    ) && !pfrom->fWhitelisted)
                {
                    LogPrint("net", "historical block serving limit reached, disconnect peer=%d\n", pfrom->GetId());

                    //disconnect node
                    pfrom->fDisconnect = true;
                    send = false;
                }
                // Pruned nodes may have deleted the block, so check whether
                // it's available before trying to send.
                if (send && (mi->second->nStatus & BLOCK_HAVE_DATA))
                {
                    // Send block from disk
                    CBlock block;
                    if (!ReadBlockFromDisk(block, (*mi).second, consensusParams))
                        assert(!"cannot load block from disk");
                    if (inv.type == MSG_BLOCK)
                        pfrom->PushMessage("block", block);
                    else // MSG_FILTERED_BLOCK)
                    {
                        bool send = false;
                        CMerkleBlock merkleBlock;
                        {
                            LOCK(pfrom->cs_filter);
                            if (pfrom->pfilter) {
                                send = true;
                                merkleBlock = CMerkleBlock(block, *pfrom->pfilter);
                            }
                        }
                        if (send) {
                            pfrom->PushMessage("merkleblock", merkleBlock);
                            // CMerkleBlock just contains hashes, so also push any transactions in the block the client did not see
                            // This avoids hurting performance by pointlessly requiring a round-trip
                            // Note that there is currently no way for a node to request any single transactions we didn't send here -
                            // they must either disconnect and retry or request the full block.
                            // Thus, the protocol spec specified allows for us to provide duplicate txn here,
                            // however we MUST always provide at least what the remote peer needs
                            typedef std::pair<unsigned int, uint256> PairType;
                            for (PairType& pair : merkleBlock.vMatchedTxn)
                                pfrom->PushMessage("tx", block.vtx[pair.first]);
                        }
                        // else
                            // no response
                    }

                    // Trigger the peer node to send a getblocks request for the next batch of inventory
                    if (inv.hash == pfrom->hashContinue)
                    {
                        // Bypass PushInventory, this must send even if redundant,
                        // and we want it right after the last block so they don't
                        // wait for other stuff first.
                        vector<CInv> vInv;
                        vInv.push_back(CInv(MSG_BLOCK, chainActive.Tip()->GetBlockHash()));
                        pfrom->PushMessage("inv", vInv);
                        pfrom->hashContinue.SetNull();
                    }
                }
            }
            else if (inv.IsKnownType())
            {
                // Check the mempool to see if a transaction is expiring soon.  If so, do not send to peer.
                // Note that a transaction enters the mempool first, before the serialized form is cached
                // in mapRelay after a successful relay.
                bool isExpiringSoon = false;
                bool pushed = false;
                CTransaction tx;
                bool isInMempool = mempool.lookup(inv.hash, tx);
                if (isInMempool) {
                    isExpiringSoon = IsExpiringSoonTx(tx, currentHeight + 1);
                }

                if (!isExpiringSoon) {
                    // Send stream from relay memory
                    {
                        LOCK(cs_mapRelay);
                        map<uint256, CTransaction>::iterator mi = mapRelay.find(inv.hash);
                        if (mi != mapRelay.end()) {
                            pfrom->PushMessage(inv.GetCommand(), (*mi).second);
                            pushed = true;
                        }
                    }
                    if (!pushed && inv.type == MSG_TX) {
                        if (isInMempool) {
                            pfrom->PushMessage("tx", tx);
                            pushed = true;
                        }
                    }
                }

                if (!pushed) {
                    vNotFound.push_back(inv);
                }
            }

            // Track requests for our stuff.
            GetMainSignals().Inventory(inv.hash);

            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK)
                break;
        }
    }

    pfrom->vRecvGetData.erase(pfrom->vRecvGetData.begin(), it);

    if (!vNotFound.empty()) {
        // Let the peer know that we didn't find what it asked for, so it doesn't
        // have to wait around forever. Currently only SPV clients actually care
        // about this message: it's needed when they are recursively walking the
        // dependencies of relevant unconfirmed transactions. SPV clients want to
        // do that because they want to know about (and store and rebroadcast and
        // risk analyze) the dependencies of transactions relevant to them, without
        // having to download the entire memory pool.
        pfrom->PushMessage("notfound", vNotFound);
    }
}

bool static ProcessMessage(const CChainParams& chainparams, CNode* pfrom, string strCommand, CDataStream& vRecv, int64_t nTimeReceived)
{
    LogPrint("net", "received: %s (%u bytes) peer=%d\n", SanitizeString(strCommand), vRecv.size(), pfrom->id);
    if (mapArgs.count("-dropmessagestest") && GetRand(atoi(mapArgs["-dropmessagestest"])) == 0)
    {
        LogPrintf("dropmessagestest DROPPING RECV MESSAGE\n");
        return true;
    }


    if (strCommand == "version")
    {
        // Each connection can only send one version message
        if (pfrom->nVersion != 0)
        {
            pfrom->PushMessage("reject", strCommand, REJECT_DUPLICATE, string("Duplicate version message"));
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 1);
            return false;
        }

        int64_t nTime;
        CAddress addrMe;
        CAddress addrFrom;
        uint64_t nNonce = 1;
        std::string strSubVer;
        std::string cleanSubVer;
        uint64_t nServices;
        vRecv >> pfrom->nVersion >> nServices >> nTime >> addrMe;
        pfrom->nServices = nServices;
        if (pfrom->nVersion < MIN_PEER_PROTO_VERSION)
        {
            // disconnect from peers older than this proto version
            LogPrintf("peer=%d using obsolete version %i; disconnecting\n", pfrom->id, pfrom->nVersion);
            pfrom->PushMessage("reject", strCommand, REJECT_OBSOLETE,
                               strprintf("Version must be %d or greater", MIN_PEER_PROTO_VERSION));
            pfrom->fDisconnect = true;
            return false;
        }

        // Reject incoming connections from nodes that don't know about the current epoch
        const Consensus::Params& consensusParams = chainparams.GetConsensus();
        auto currentEpoch = CurrentEpoch(GetHeight(), consensusParams);
        if (pfrom->nVersion < consensusParams.vUpgrades[currentEpoch].nProtocolVersion &&
            !(
                chainparams.NetworkIDString() == "regtest" &&
                !GetBoolArg("-nurejectoldversions", DEFAULT_NU_REJECT_OLD_VERSIONS)
            )
        ) {
            LogPrintf("peer=%d using obsolete version %i; disconnecting\n", pfrom->id, pfrom->nVersion);
            pfrom->PushMessage("reject", strCommand, REJECT_OBSOLETE,
                            strprintf("Version must be %d or greater",
                            consensusParams.vUpgrades[currentEpoch].nProtocolVersion));
            pfrom->fDisconnect = true;
            return false;
        }

        if (pfrom->nVersion == 10300)
            pfrom->nVersion = 300;
        if (!vRecv.empty())
            vRecv >> addrFrom >> nNonce;
        if (!vRecv.empty()) {
            vRecv >> LIMITED_STRING(strSubVer, MAX_SUBVERSION_LENGTH);
            cleanSubVer = SanitizeString(strSubVer, SAFE_CHARS_SUBVERSION);
        }
        if (!vRecv.empty()) {
            int nStartingHeight;
            vRecv >> nStartingHeight;
            pfrom->nStartingHeight = nStartingHeight;
        }
        if (!vRecv.empty())
            vRecv >> pfrom->fRelayTxes; // set to true after we get the first filter* message
        else
            pfrom->fRelayTxes = true;

        // Disconnect if we connected to ourself
        if (nNonce == nLocalHostNonce && nNonce > 1)
        {
            LogPrintf("connected to self at %s, disconnecting\n", pfrom->addr.ToString());
            pfrom->fDisconnect = true;
            return true;
        }

        pfrom->SetAddrLocal(addrMe);
        if (pfrom->fInbound && addrMe.IsRoutable())
        {
            SeenLocal(addrMe);
        }

        // Be shy and don't send version until we hear
        if (pfrom->fInbound)
            pfrom->PushVersion();

        {
            LOCK(pfrom->cs_SubVer);
            pfrom->strSubVer = strSubVer;
            pfrom->cleanSubVer = cleanSubVer;
        }
        pfrom->fClient = !(pfrom->nServices & NODE_NETWORK);

        // Potentially mark this peer as a preferred download peer.
        {
            LOCK(cs_main);
            UpdatePreferredDownload(pfrom, State(pfrom->GetId()));
        }

        // Change version
        pfrom->PushMessage("verack");
        pfrom->ssSend.SetVersion(min(pfrom->nVersion, PROTOCOL_VERSION));

        if (!pfrom->fInbound)
        {
            // Advertise our address
            if (fListen && !IsInitialBlockDownload(chainparams.GetConsensus()))
            {
                CAddress addr = GetLocalAddress(&pfrom->addr);
                FastRandomContext insecure_rand;
                if (addr.IsRoutable())
                {
                    LogPrintf("ProcessMessages: advertizing address %s\n", addr.ToString());
                    pfrom->PushAddress(addr, insecure_rand);
                } else if (IsPeerAddrLocalGood(pfrom)) {
                    addr.SetIP(addrMe);
                    LogPrintf("ProcessMessages: advertizing address %s\n", addr.ToString());
                    pfrom->PushAddress(addr, insecure_rand);
                }
            }

            // Get recent addresses
            if (pfrom->fOneShot || pfrom->nVersion >= CADDR_TIME_VERSION || addrman.size() < 1000)
            {
                pfrom->PushMessage("getaddr");
                pfrom->fGetAddr = true;
            }
            addrman.Good(pfrom->addr);
        } else {
            if (((CNetAddr)pfrom->addr) == (CNetAddr)addrFrom)
            {
                addrman.Add(addrFrom, addrFrom);
                addrman.Good(addrFrom);
            }
        }

        // Relay alerts
        {
            LOCK(cs_mapAlerts);
            for (std::pair<const uint256, CAlert>& item : mapAlerts)
                item.second.RelayTo(pfrom);
        }

        pfrom->fSuccessfullyConnected = true;

        string remoteAddr;
        if (fLogIPs)
            remoteAddr = ", peeraddr=" + pfrom->addr.ToString();

        LogPrintf("receive version message: %s: version %d, blocks=%d, us=%s, peer=%d%s\n",
                  cleanSubVer, pfrom->nVersion,
                  pfrom->nStartingHeight, addrMe.ToString(), pfrom->id,
                  remoteAddr);

        pfrom->nTimeOffset = timeWarning.AddTimeData(pfrom->addr, nTime, GetTime());
    }


    else if (pfrom->nVersion == 0)
    {
        // Must have a version message before anything else
        LOCK(cs_main);
        Misbehaving(pfrom->GetId(), 1);
        return false;
    }


    else if (strCommand == "verack")
    {
        pfrom->SetRecvVersion(min(pfrom->nVersion, PROTOCOL_VERSION));

        // Mark this node as currently connected, so we update its timestamp later.
        if (pfrom->fNetworkNode) {
            LOCK(cs_main);
            State(pfrom->GetId())->fCurrentlyConnected = true;
        }
    }


    // Disconnect existing peer connection when:
    // 1. The version message has been received
    // 2. Peer version is below the minimum version for the current epoch
    else if (pfrom->nVersion < chainparams.GetConsensus().vUpgrades[
        CurrentEpoch(GetHeight(), chainparams.GetConsensus())].nProtocolVersion &&
        !(
            chainparams.NetworkIDString() == "regtest" &&
            !GetBoolArg("-nurejectoldversions", DEFAULT_NU_REJECT_OLD_VERSIONS)
        )
    ) {
        LogPrintf("peer=%d using obsolete version %i; disconnecting\n", pfrom->id, pfrom->nVersion);
        pfrom->PushMessage("reject", strCommand, REJECT_OBSOLETE,
                            strprintf("Version must be %d or greater",
                            chainparams.GetConsensus().vUpgrades[
                                CurrentEpoch(GetHeight(), chainparams.GetConsensus())].nProtocolVersion));
        pfrom->fDisconnect = true;
        return false;
    }


    else if (strCommand == "addr")
    {
        vector<CAddress> vAddr;
        vRecv >> vAddr;

        // Don't want addr from older versions unless seeding
        if (pfrom->nVersion < CADDR_TIME_VERSION && addrman.size() > 1000)
            return true;
        if (vAddr.size() > 1000)
        {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 20);
            return error("message addr size() = %u", vAddr.size());
        }

        // Store the new addresses
        vector<CAddress> vAddrOk;
        int64_t nNow = GetTime();
        int64_t nSince = nNow - 10 * 60;
        for (CAddress& addr : vAddr)
        {
            boost::this_thread::interruption_point();

            if (addr.nTime <= 100000000 || addr.nTime > nNow + 10 * 60)
                addr.nTime = nNow - 5 * 24 * 60 * 60;
            pfrom->AddAddressKnown(addr);
            bool fReachable = IsReachable(addr);
            if (addr.nTime > nSince && !pfrom->fGetAddr && vAddr.size() <= 10 && addr.IsRoutable())
            {
                // Relay to a limited number of other nodes
                {
                    LOCK(cs_vNodes);
                    // Use deterministic randomness to send to the same nodes for 24 hours
                    // at a time so the addrKnowns of the chosen nodes prevent repeats
                    static const uint64_t salt0 = GetRand(std::numeric_limits<uint64_t>::max());
                    static const uint64_t salt1 = GetRand(std::numeric_limits<uint64_t>::max());
                    uint64_t hashAddr = addr.GetHash();
                    multimap<uint64_t, CNode*> mapMix;
                    FastRandomContext insecure_rand;
                    const CSipHasher hasher = CSipHasher(salt0, salt1).Write(hashAddr << 32).Write((GetTime() + hashAddr) / (24*60*60));
                    for (CNode* pnode : vNodes)
                    {
                        if (pnode->nVersion < CADDR_TIME_VERSION)
                            continue;
                        uint64_t hashKey = CSipHasher(hasher).Write(pnode->id).Finalize();
                        mapMix.insert(make_pair(hashKey, pnode));
                    }
                    int nRelayNodes = fReachable ? 2 : 1; // limited relaying of addresses outside our network(s)
                    for (multimap<uint64_t, CNode*>::iterator mi = mapMix.begin(); mi != mapMix.end() && nRelayNodes-- > 0; ++mi)
                        ((*mi).second)->PushAddress(addr, insecure_rand);
                }
            }
            // Do not store addresses outside our network
            if (fReachable)
                vAddrOk.push_back(addr);
        }
        addrman.Add(vAddrOk, pfrom->addr, 2 * 60 * 60);
        if (vAddr.size() < 1000)
            pfrom->fGetAddr = false;
        if (pfrom->fOneShot)
            pfrom->fDisconnect = true;
    }


    else if (strCommand == "inv")
    {
        vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ)
        {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 20);
            return error("message inv size() = %u", vInv.size());
        }

        bool fBlocksOnly = GetBoolArg("-blocksonly", DEFAULT_BLOCKSONLY);

        // Allow whitelisted peers to send data other than blocks in blocks only mode if whitelistrelay is true
        if (pfrom->fWhitelisted && GetBoolArg("-whitelistrelay", DEFAULT_WHITELISTRELAY))
            fBlocksOnly = false;

        LOCK(cs_main);

        std::vector<CInv> vToFetch;

        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++)
        {
            const CInv &inv = vInv[nInv];

            boost::this_thread::interruption_point();
            pfrom->AddInventoryKnown(inv);

            bool fAlreadyHave = AlreadyHave(inv);
            LogPrint("net", "got inv: %s  %s peer=%d\n", inv.ToString(), fAlreadyHave ? "have" : "new", pfrom->id);

            if (inv.type == MSG_BLOCK) {
                UpdateBlockAvailability(pfrom->GetId(), inv.hash);
                if (!fAlreadyHave && !fImporting && !fReindex && !mapBlocksInFlight.count(inv.hash)) {
                    // First request the headers preceding the announced block. In the normal fully-synced
                    // case where a new block is announced that succeeds the current tip (no reorganization),
                    // there are no such headers.
                    // Secondly, and only when we are close to being synced, we request the announced block directly,
                    // to avoid an extra round-trip. Note that we must *first* ask for the headers, so by the
                    // time the block arrives, the header chain leading up to it is already validated. Not
                    // doing this will result in the received block being rejected as an orphan in case it is
                    // not a direct successor.
                    pfrom->PushMessage("getheaders", chainActive.GetLocator(pindexBestHeader), inv.hash);
                    CNodeState *nodestate = State(pfrom->GetId());

                    if (chainActive.Tip()->GetBlockTime() > GetTime() - chainparams.GetConsensus().PoWTargetSpacing(pindexBestHeader->nHeight) * 20 &&
                        nodestate->nBlocksInFlight < MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
                        vToFetch.push_back(inv);
                        // Mark block as in flight already, even though the actual "getdata" message only goes out
                        // later (within the same cs_main lock, though).
                        MarkBlockAsInFlight(pfrom->GetId(), inv.hash, chainparams.GetConsensus());
                    }
                    LogPrint("net", "getheaders (%d) %s to peer=%d\n", pindexBestHeader->nHeight, inv.hash.ToString(), pfrom->id);
                }
            }
            else
            {
                if (fBlocksOnly)
                    LogPrint("net", "transaction (%s) inv sent in violation of protocol peer=%d\n", inv.hash.ToString(), pfrom->id);
                else if (!fAlreadyHave && !IsInitialBlockDownload(chainparams.GetConsensus()))
                    pfrom->AskFor(inv);
            }

            // Track requests for our stuff
            GetMainSignals().Inventory(inv.hash);

            if (pfrom->nSendSize > (SendBufferSize() * 2)) {
                Misbehaving(pfrom->GetId(), 50);
                return error("send buffer size() = %u", pfrom->nSendSize);
            }
        }

        if (!vToFetch.empty())
            pfrom->PushMessage("getdata", vToFetch);
    }


    else if (strCommand == "getdata")
    {
        vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ)
        {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 20);
            return error("message getdata size() = %u", vInv.size());
        }

        if (fDebug || (vInv.size() != 1))
            LogPrint("net", "received getdata (%u invsz) peer=%d\n", vInv.size(), pfrom->id);

        if ((fDebug && vInv.size() > 0) || (vInv.size() == 1))
            LogPrint("net", "received getdata for: %s peer=%d\n", vInv[0].ToString(), pfrom->id);

        pfrom->vRecvGetData.insert(pfrom->vRecvGetData.end(), vInv.begin(), vInv.end());
        ProcessGetData(pfrom, chainparams.GetConsensus());
    }


    else if (strCommand == "getblocks")
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        LOCK(cs_main);

        // Find the last block the caller has in the main chain
        CBlockIndex* pindex = FindForkInGlobalIndex(chainActive, locator);

        // Send the rest of the chain
        if (pindex)
            pindex = chainActive.Next(pindex);
        int nLimit = 500;
        LogPrint("net", "getblocks %d to %s limit %d from peer=%d\n", (pindex ? pindex->nHeight : -1), hashStop.IsNull() ? "end" : hashStop.ToString(), nLimit, pfrom->id);
        for (; pindex; pindex = chainActive.Next(pindex))
        {
            if (pindex->GetBlockHash() == hashStop)
            {
                LogPrint("net", "  getblocks stopping at %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                break;
            }
            // If pruning, don't inv blocks unless we have on disk and are likely to still have
            // for some reasonable time window (1 hour) that block relay might require.
            const int nPrunedBlocksLikelyToHave = MIN_BLOCKS_TO_KEEP - 3600 / chainparams.GetConsensus().PoWTargetSpacing(pindex->nHeight);
            if (fPruneMode && (!(pindex->nStatus & BLOCK_HAVE_DATA) || pindex->nHeight <= chainActive.Tip()->nHeight - nPrunedBlocksLikelyToHave))
            {
                LogPrint("net", " getblocks stopping, pruned or too old block at %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                break;
            }
            pfrom->PushInventory(CInv(MSG_BLOCK, pindex->GetBlockHash()));
            if (--nLimit <= 0)
            {
                // When this block is requested, we'll send an inv that'll
                // trigger the peer to getblocks the next batch of inventory.
                LogPrint("net", "  getblocks stopping at limit %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                pfrom->hashContinue = pindex->GetBlockHash();
                break;
            }
        }
    }


    else if (strCommand == "getheaders")
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        LOCK(cs_main);
        if (IsInitialBlockDownload(chainparams.GetConsensus()) && !pfrom->fWhitelisted) {
            LogPrint("net", "Ignoring getheaders from peer=%d because node is in initial block download\n", pfrom->id);
            return true;
        }
        CBlockIndex* pindex = NULL;
        if (locator.IsNull())
        {
            // If locator is null, return the hashStop block
            BlockMap::iterator mi = mapBlockIndex.find(hashStop);
            if (mi == mapBlockIndex.end())
                return true;
            pindex = (*mi).second;
        }
        else
        {
            // Find the last block the caller has in the main chain
            pindex = FindForkInGlobalIndex(chainActive, locator);
            if (pindex)
                pindex = chainActive.Next(pindex);
        }

        // we must use CBlocks, as CBlockHeaders won't include the 0x00 nTx count at the end
        vector<CBlock> vHeaders;
        int nLimit = MAX_HEADERS_RESULTS;
        LogPrint("net", "getheaders %d to %s from peer=%d\n", (pindex ? pindex->nHeight : -1), hashStop.ToString(), pfrom->id);
        for (; pindex; pindex = chainActive.Next(pindex))
        {
            vHeaders.push_back(pindex->GetBlockHeader());
            if (--nLimit <= 0 || pindex->GetBlockHash() == hashStop)
                break;
        }
        pfrom->PushMessage("headers", vHeaders);
    }


    else if (strCommand == "tx" && !IsInitialBlockDownload(chainparams.GetConsensus()))
    {
        // Stop processing the transaction early if
        // We are in blocks only mode and peer is either not whitelisted or whitelistrelay is off
        if (GetBoolArg("-blocksonly", DEFAULT_BLOCKSONLY) && (!pfrom->fWhitelisted || !GetBoolArg("-whitelistrelay", DEFAULT_WHITELISTRELAY)))
        {
            LogPrint("net", "transaction sent in violation of protocol peer=%d\n", pfrom->id);
            return true;
        }

        vector<uint256> vWorkQueue;
        vector<uint256> vEraseQueue;
        CTransaction tx;
        vRecv >> tx;

        CInv inv(MSG_TX, tx.GetHash());
        pfrom->AddInventoryKnown(inv);

        LOCK(cs_main);

        bool fMissingInputs = false;
        CValidationState state;

        pfrom->setAskFor.erase(inv.hash);
        mapAlreadyAskedFor.erase(inv.hash);

        if (!AlreadyHave(inv) && AcceptToMemoryPool(chainparams, mempool, state, tx, true, &fMissingInputs))
        {
            mempool.check(pcoinsTip);
            RelayTransaction(tx);
            vWorkQueue.push_back(inv.hash);

            LogPrint("mempool", "AcceptToMemoryPool: peer=%d %s: accepted %s (poolsz %u txn, %u kB)\n",
                pfrom->id, pfrom->cleanSubVer,
                tx.GetHash().ToString(),
                mempool.size(), mempool.DynamicMemoryUsage() / 1000);

            // Recursively process any orphan transactions that depended on this one
            set<NodeId> setMisbehaving;
            for (unsigned int i = 0; i < vWorkQueue.size(); i++)
            {
                map<uint256, set<uint256> >::iterator itByPrev = mapOrphanTransactionsByPrev.find(vWorkQueue[i]);
                if (itByPrev == mapOrphanTransactionsByPrev.end())
                    continue;
                for (set<uint256>::iterator mi = itByPrev->second.begin();
                     mi != itByPrev->second.end();
                     ++mi)
                {
                    const uint256& orphanHash = *mi;
                    const CTransaction& orphanTx = mapOrphanTransactions[orphanHash].tx;
                    NodeId fromPeer = mapOrphanTransactions[orphanHash].fromPeer;
                    bool fMissingInputs2 = false;
                    // Use a dummy CValidationState so someone can't setup nodes to counter-DoS based on orphan
                    // resolution (that is, feeding people an invalid transaction based on LegitTxX in order to get
                    // anyone relaying LegitTxX banned)
                    CValidationState stateDummy;


                    if (setMisbehaving.count(fromPeer))
                        continue;
                    if (AcceptToMemoryPool(chainparams, mempool, stateDummy, orphanTx, true, &fMissingInputs2))
                    {
                        LogPrint("mempool", "   accepted orphan tx %s\n", orphanHash.ToString());
                        RelayTransaction(orphanTx);
                        vWorkQueue.push_back(orphanHash);
                        vEraseQueue.push_back(orphanHash);
                    }
                    else if (!fMissingInputs2)
                    {
                        int nDos = 0;
                        if (stateDummy.IsInvalid(nDos) && nDos > 0)
                        {
                            // Punish peer that gave us an invalid orphan tx
                            Misbehaving(fromPeer, nDos);
                            setMisbehaving.insert(fromPeer);
                            LogPrint("mempool", "   invalid orphan tx %s\n", orphanHash.ToString());
                        }
                        // Has inputs but not accepted to mempool
                        // Probably non-standard or insufficient fee/priority
                        LogPrint("mempool", "   removed orphan tx %s\n", orphanHash.ToString());
                        vEraseQueue.push_back(orphanHash);
                        assert(recentRejects);
                        recentRejects->insert(orphanHash);
                    }
                    mempool.check(pcoinsTip);
                }
            }

            for (uint256 hash : vEraseQueue)
                EraseOrphanTx(hash);
        }
        // TODO: currently, prohibit joinsplits and shielded spends/outputs from entering mapOrphans
        else if (fMissingInputs &&
                 tx.vJoinSplit.empty() &&
                 tx.vShieldedSpend.empty() &&
                 tx.vShieldedOutput.empty())
        {
            AddOrphanTx(tx, pfrom->GetId());

            // DoS prevention: do not allow mapOrphanTransactions to grow unbounded
            unsigned int nMaxOrphanTx = (unsigned int)std::max((int64_t)0, GetArg("-maxorphantx", DEFAULT_MAX_ORPHAN_TRANSACTIONS));
            unsigned int nEvicted = LimitOrphanTxSize(nMaxOrphanTx);
            if (nEvicted > 0)
                LogPrint("mempool", "mapOrphan overflow, removed %u tx\n", nEvicted);
        } else {
            assert(recentRejects);
            recentRejects->insert(tx.GetHash());

            if (pfrom->fWhitelisted && GetBoolArg("-whitelistforcerelay", DEFAULT_WHITELISTFORCERELAY)) {
                // Always relay transactions received from whitelisted peers, even
                // if they were already in the mempool or rejected from it due
                // to policy, allowing the node to function as a gateway for
                // nodes hidden behind it.
                //
                // Never relay transactions that we would assign a non-zero DoS
                // score for, as we expect peers to do the same with us in that
                // case.
                int nDoS = 0;
                if (!state.IsInvalid(nDoS) || nDoS == 0) {
                    LogPrintf("Force relaying tx %s from whitelisted peer=%d\n", tx.GetHash().ToString(), pfrom->id);
                    RelayTransaction(tx);
                } else {
                    LogPrintf("Not relaying invalid transaction %s from whitelisted peer=%d (%s (code %d))\n",
                        tx.GetHash().ToString(), pfrom->id, state.GetRejectReason(), state.GetRejectCode());
                }
            }
        }
        int nDoS = 0;
        if (state.IsInvalid(nDoS))
        {
            LogPrint("mempool", "%s from peer=%d %s was not accepted into the memory pool: %s\n", tx.GetHash().ToString(),
                pfrom->id, pfrom->cleanSubVer,
                state.GetRejectReason());
            pfrom->PushMessage("reject", strCommand, state.GetRejectCode(),
                               state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH), inv.hash);
            if (nDoS > 0)
                Misbehaving(pfrom->GetId(), nDoS);
        }
    }


    else if (strCommand == "headers" && !fImporting && !fReindex) // Ignore headers received while importing
    {
        std::vector<CBlockHeader> headers;

        // Bypass the normal CBlock deserialization, as we don't want to risk deserializing 2000 full blocks.
        unsigned int nCount = ReadCompactSize(vRecv);
        if (nCount > MAX_HEADERS_RESULTS) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 20);
            return error("headers message size = %u", nCount);
        }
        headers.resize(nCount);
        for (unsigned int n = 0; n < nCount; n++) {
            vRecv >> headers[n];
            ReadCompactSize(vRecv); // ignore tx count; assume it is 0.
        }

        {
        LOCK(cs_main);

        if (nCount == 0) {
            // Nothing interesting. Stop asking this peer for more headers.
            return true;
        }

        // If we already know the last header in the message, then it contains
        // no new information for us.  In this case, we do not request
        // more headers later.  This prevents multiple chains of redundant
        // getheader requests from running in parallel if triggered by incoming
        // blocks while the node is still in initial headers sync.
        //
        // (Allow disabling optimization in case there are unexpected problems.)
        bool hasNewHeaders = true;
        if (GetBoolArg("-optimize-getheaders", true) && IsInitialBlockDownload(chainparams.GetConsensus())) {
            hasNewHeaders = (mapBlockIndex.count(headers.back().GetHash()) == 0);
        }

        CBlockIndex *pindexLast = NULL;
        for (const CBlockHeader& header : headers) {
            CValidationState state;
            if (pindexLast != NULL && header.hashPrevBlock != pindexLast->GetBlockHash()) {
                Misbehaving(pfrom->GetId(), 20);
                return error("non-continuous headers sequence");
            }
            if (!AcceptBlockHeader(header, state, chainparams, &pindexLast)) {
                int nDoS;
                if (state.IsInvalid(nDoS)) {
                    if (nDoS > 0)
                        Misbehaving(pfrom->GetId(), nDoS);
                    return error("invalid header received");
                }
            }
        }

        if (pindexLast)
            UpdateBlockAvailability(pfrom->GetId(), pindexLast->GetBlockHash());

        // Temporary, until we're sure the optimization works
        if (nCount == MAX_HEADERS_RESULTS && pindexLast && !hasNewHeaders) {
            LogPrint("net", "NO more getheaders (%d) to send to peer=%d (startheight:%d)\n", pindexLast->nHeight, pfrom->id, pfrom->nStartingHeight);
        }

        if (nCount == MAX_HEADERS_RESULTS && pindexLast && hasNewHeaders) {
            // Headers message had its maximum size; the peer may have more headers.
            // TODO: optimize: if pindexLast is an ancestor of chainActive.Tip or pindexBestHeader, continue
            // from there instead.
            LogPrint("net", "more getheaders (%d) to send to peer=%d (startheight:%d)\n", pindexLast->nHeight, pfrom->id, pfrom->nStartingHeight);
            pfrom->PushMessage("getheaders", chainActive.GetLocator(pindexLast), uint256());
        }

        CheckBlockIndex(chainparams.GetConsensus());
        }

        NotifyHeaderTip(chainparams.GetConsensus());
    }

    else if (strCommand == "block" && !fImporting && !fReindex) // Ignore blocks received while importing
    {
        CBlock block;
        vRecv >> block;

        CInv inv(MSG_BLOCK, block.GetHash());
        LogPrint("net", "received block %s peer=%d\n", inv.hash.ToString(), pfrom->id);

        pfrom->AddInventoryKnown(inv);

        CValidationState state;
        // Process all blocks from whitelisted peers, even if not requested,
        // unless we're still syncing with the network.
        // Such an unrequested block may still be processed, subject to the
        // conditions in AcceptBlock().
        bool forceProcessing = pfrom->fWhitelisted && !IsInitialBlockDownload(chainparams.GetConsensus());
        ProcessNewBlock(state, chainparams, pfrom, &block, forceProcessing, NULL);
        int nDoS;
        if (state.IsInvalid(nDoS)) {
            pfrom->PushMessage("reject", strCommand, state.GetRejectCode(),
                               state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH), inv.hash);
            if (nDoS > 0) {
                LOCK(cs_main);
                Misbehaving(pfrom->GetId(), nDoS);
            }
        }

    }


    // This asymmetric behavior for inbound and outbound connections was introduced
    // to prevent a fingerprinting attack: an attacker can send specific fake addresses
    // to users' AddrMan and later request them by sending getaddr messages.
    // Making nodes which are behind NAT and can only make outgoing connections ignore
    // the getaddr message mitigates the attack.
    else if ((strCommand == "getaddr") && (pfrom->fInbound))
    {
        // Only send one GetAddr response per connection to reduce resource waste
        //  and discourage addr stamping of INV announcements.
        if (pfrom->fSentAddr) {
            LogPrint("net", "Ignoring repeated \"getaddr\". peer=%d\n", pfrom->id);
            return true;
        }
        pfrom->fSentAddr = true;

        pfrom->vAddrToSend.clear();
        vector<CAddress> vAddr = addrman.GetAddr();
        FastRandomContext insecure_rand;
        for (const CAddress &addr : vAddr)
            pfrom->PushAddress(addr, insecure_rand);
    }


    else if (strCommand == "mempool")
    {
        int currentHeight = GetHeight();
        if (CNode::OutboundTargetReached(chainparams.GetConsensus().PoWTargetSpacing(currentHeight), false) && !pfrom->fWhitelisted)
        {
            LogPrint("net", "mempool request with bandwidth limit reached, disconnect peer=%d\n", pfrom->GetId());
            pfrom->fDisconnect = true;
            return true;
        }

        LOCK2(cs_main, pfrom->cs_filter);

        std::vector<uint256> vtxid;
        mempool.queryHashes(vtxid);
        vector<CInv> vInv;
        for (uint256& hash : vtxid) {
            CTransaction tx;
            bool fInMemPool = mempool.lookup(hash, tx);
            if (fInMemPool && IsExpiringSoonTx(tx, currentHeight + 1)) {
                continue;
            }

            CInv inv(MSG_TX, hash);
            if (pfrom->pfilter) {
                if (!fInMemPool) continue; // another thread removed since queryHashes, maybe...
                if (!pfrom->pfilter->IsRelevantAndUpdate(tx)) continue;
            }
            vInv.push_back(inv);
            if (vInv.size() == MAX_INV_SZ) {
                pfrom->PushMessage("inv", vInv);
                vInv.clear();
            }
        }
        if (vInv.size() > 0)
            pfrom->PushMessage("inv", vInv);
    }


    else if (strCommand == "ping")
    {
        if (pfrom->nVersion > BIP0031_VERSION)
        {
            uint64_t nonce = 0;
            vRecv >> nonce;
            // Echo the message back with the nonce. This allows for two useful features:
            //
            // 1) A remote node can quickly check if the connection is operational
            // 2) Remote nodes can measure the latency of the network thread. If this node
            //    is overloaded it won't respond to pings quickly and the remote node can
            //    avoid sending us more work, like chain download requests.
            //
            // The nonce stops the remote getting confused between different pings: without
            // it, if the remote node sends a ping once per second and this node takes 5
            // seconds to respond to each, the 5th ping the remote sends would appear to
            // return very quickly.
            pfrom->PushMessage("pong", nonce);
        }
    }


    else if (strCommand == "pong")
    {
        int64_t pingUsecEnd = nTimeReceived;
        uint64_t nonce = 0;
        size_t nAvail = vRecv.in_avail();
        bool bPingFinished = false;
        std::string sProblem;

        if (nAvail >= sizeof(nonce)) {
            vRecv >> nonce;

            // Only process pong message if there is an outstanding ping (old ping without nonce should never pong)
            if (pfrom->nPingNonceSent != 0) {
                if (nonce == pfrom->nPingNonceSent) {
                    // Matching pong received, this ping is no longer outstanding
                    bPingFinished = true;
                    int64_t pingUsecTime = pingUsecEnd - pfrom->nPingUsecStart;
                    if (pingUsecTime > 0) {
                        // Successful ping time measurement, replace previous
                        pfrom->nPingUsecTime = pingUsecTime;
                        pfrom->nMinPingUsecTime = std::min(pfrom->nMinPingUsecTime.load(), pingUsecTime);
                    } else {
                        // This should never happen
                        sProblem = "Timing mishap";
                    }
                } else {
                    // Nonce mismatches are normal when pings are overlapping
                    sProblem = "Nonce mismatch";
                    if (nonce == 0) {
                        // This is most likely a bug in another implementation somewhere; cancel this ping
                        bPingFinished = true;
                        sProblem = "Nonce zero";
                    }
                }
            } else {
                sProblem = "Unsolicited pong without ping";
            }
        } else {
            // This is most likely a bug in another implementation somewhere; cancel this ping
            bPingFinished = true;
            sProblem = "Short payload";
        }

        if (!(sProblem.empty())) {
            LogPrint("net", "pong peer=%d %s: %s, %x expected, %x received, %u bytes\n",
                pfrom->id,
                pfrom->cleanSubVer,
                sProblem,
                pfrom->nPingNonceSent,
                nonce,
                nAvail);
        }
        if (bPingFinished) {
            pfrom->nPingNonceSent = 0;
        }
    }


    else if (fAlerts && strCommand == "alert")
    {
        CAlert alert;
        vRecv >> alert;

        uint256 alertHash = alert.GetHash();
        if (pfrom->setKnown.count(alertHash) == 0)
        {
            if (alert.ProcessAlert(chainparams.AlertKey()))
            {
                // Relay
                pfrom->setKnown.insert(alertHash);
                {
                    LOCK(cs_vNodes);
                    for (CNode* pnode : vNodes)
                        alert.RelayTo(pnode);
                }
            }
            else {
                // Small DoS penalty so peers that send us lots of
                // duplicate/expired/invalid-signature/whatever alerts
                // eventually get banned.
                // This isn't a Misbehaving(100) (immediate ban) because the
                // peer might be an older or different implementation with
                // a different signature key, etc.
                LOCK(cs_main);
                Misbehaving(pfrom->GetId(), 10);
            }
        }
    }


    else if (!(nLocalServices & NODE_BLOOM) &&
              (strCommand == "filterload" ||
               strCommand == "filteradd"))
    {
        if (pfrom->nVersion >= NO_BLOOM_VERSION) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 100);
            return false;
        } else if (GetBoolArg("-enforcenodebloom", DEFAULT_ENFORCENODEBLOOM)) {
            pfrom->fDisconnect = true;
            return false;
        }
    }


    else if (strCommand == "filterload")
    {
        CBloomFilter filter;
        vRecv >> filter;

        if (!filter.IsWithinSizeConstraints())
        {
            // There is no excuse for sending a too-large filter
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 100);
        }
        else
        {
            LOCK(pfrom->cs_filter);
            delete pfrom->pfilter;
            pfrom->pfilter = new CBloomFilter(filter);
            pfrom->fRelayTxes = true;
        }
    }


    else if (strCommand == "filteradd")
    {
        vector<unsigned char> vData;
        vRecv >> vData;

        // Nodes must NEVER send a data item > 520 bytes (the max size for a script data object,
        // and thus, the maximum size any matched object can have) in a filteradd message
        bool bad = false;
        if (vData.size() > MAX_SCRIPT_ELEMENT_SIZE) {
            bad = true;
        } else {
            LOCK(pfrom->cs_filter);
            if (pfrom->pfilter) {
                pfrom->pfilter->insert(vData);
            } else {
                bad = true;
            }
        }
        if (bad) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 100);
        }
    }


    else if (strCommand == "filterclear")
    {
        LOCK(pfrom->cs_filter);
        if (nLocalServices & NODE_BLOOM) {
            delete pfrom->pfilter;
            pfrom->pfilter = new CBloomFilter();
        }
        pfrom->fRelayTxes = true;
    }


    else if (strCommand == "reject")
    {
        if (fDebug) {
            try {
                string strMsg; unsigned char ccode; string strReason;
                vRecv >> LIMITED_STRING(strMsg, CMessageHeader::COMMAND_SIZE) >> ccode >> LIMITED_STRING(strReason, MAX_REJECT_MESSAGE_LENGTH);

                ostringstream ss;
                ss << strMsg << " code " << itostr(ccode) << ": " << strReason;

                if (strMsg == "block" || strMsg == "tx")
                {
                    uint256 hash;
                    vRecv >> hash;
                    ss << ": hash " << hash.ToString();
                }
                LogPrint("net", "Reject %s\n", SanitizeString(ss.str()));
            } catch (const std::ios_base::failure&) {
                // Avoid feedback loops by preventing reject messages from triggering a new reject message.
                LogPrint("net", "Unparseable reject message received\n");
            }
        }
    }

    else if (strCommand == "notfound") {
        // We do not care about the NOTFOUND message, but logging an Unknown Command
        // message would be undesirable as we transmit it ourselves.
    }

    else if (!(strCommand == "tx" || strCommand == "block" || strCommand == "headers" || strCommand == "alert")) {
        // Ignore unknown commands for extensibility
        LogPrint("net", "Unknown command \"%s\" from peer=%d\n", SanitizeString(strCommand), pfrom->id);
    }



    return true;
}

// requires LOCK(cs_vRecvMsg)
bool ProcessMessages(const CChainParams& chainparams, CNode* pfrom)
{
    //if (fDebug)
    //    LogPrintf("%s(%u messages)\n", __func__, pfrom->vRecvMsg.size());

    //
    // Message format
    //  (4) message start
    //  (12) command
    //  (4) size
    //  (4) checksum
    //  (x) data
    //
    bool fOk = true;

    if (!pfrom->vRecvGetData.empty())
        ProcessGetData(pfrom, chainparams.GetConsensus());

    // this maintains the order of responses
    if (!pfrom->vRecvGetData.empty()) return fOk;

    std::deque<CNetMessage>::iterator it = pfrom->vRecvMsg.begin();
    while (!pfrom->fDisconnect && it != pfrom->vRecvMsg.end()) {
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize())
            break;

        // get next message
        CNetMessage& msg = *it;

        //if (fDebug)
        //    LogPrintf("%s(message %u msgsz, %u bytes, complete:%s)\n", __func__,
        //            msg.hdr.nMessageSize, msg.vRecv.size(),
        //            msg.complete() ? "Y" : "N");

        // end, if an incomplete message is found
        if (!msg.complete())
            break;

        // at this point, any failure means we can delete the current message
        it++;

        // Scan for message start
        if (memcmp(msg.hdr.pchMessageStart, chainparams.MessageStart(), MESSAGE_START_SIZE) != 0) {
            LogPrintf("PROCESSMESSAGE: INVALID MESSAGESTART %s peer=%d\n", SanitizeString(msg.hdr.GetCommand()), pfrom->id);
            fOk = false;
            break;
        }

        // Read header
        CMessageHeader& hdr = msg.hdr;
        if (!hdr.IsValid(chainparams.MessageStart()))
        {
            LogPrintf("PROCESSMESSAGE: ERRORS IN HEADER %s peer=%d\n", SanitizeString(hdr.GetCommand()), pfrom->id);
            continue;
        }
        string strCommand = hdr.GetCommand();

        // Message size
        unsigned int nMessageSize = hdr.nMessageSize;

        // Checksum
        CDataStream& vRecv = msg.vRecv;
        uint256 hash = Hash(vRecv.begin(), vRecv.begin() + nMessageSize);
        unsigned int nChecksum = ReadLE32((unsigned char*)&hash);
        if (nChecksum != hdr.nChecksum)
        {
            LogPrintf("%s(%s, %u bytes): CHECKSUM ERROR nChecksum=%08x hdr.nChecksum=%08x\n", __func__,
               SanitizeString(strCommand), nMessageSize, nChecksum, hdr.nChecksum);
            continue;
        }

        // Process message
        bool fRet = false;
        try
        {
            fRet = ProcessMessage(chainparams, pfrom, strCommand, vRecv, msg.nTime);
            boost::this_thread::interruption_point();
        }
        catch (const std::ios_base::failure& e)
        {
            pfrom->PushMessage("reject", strCommand, REJECT_MALFORMED, string("error parsing message"));
            if (strstr(e.what(), "end of data"))
            {
                // Allow exceptions from under-length message on vRecv
                LogPrintf("%s(%s, %u bytes): Exception '%s' caught, normally caused by a message being shorter than its stated length\n", __func__, SanitizeString(strCommand), nMessageSize, e.what());
            }
            else if (strstr(e.what(), "size too large"))
            {
                // Allow exceptions from over-long size
                LogPrintf("%s(%s, %u bytes): Exception '%s' caught\n", __func__, SanitizeString(strCommand), nMessageSize, e.what());
            }
            else
            {
                PrintExceptionContinue(&e, "ProcessMessages()");
            }
        }
        catch (const boost::thread_interrupted&) {
            throw;
        }
        catch (const std::exception& e) {
            PrintExceptionContinue(&e, "ProcessMessages()");
        } catch (...) {
            PrintExceptionContinue(NULL, "ProcessMessages()");
        }

        if (!fRet)
            LogPrintf("%s(%s, %u bytes) FAILED peer=%d\n", __func__, SanitizeString(strCommand), nMessageSize, pfrom->id);

        break;
    }

    // In case the connection got shut down, its receive buffer was wiped
    if (!pfrom->fDisconnect)
        pfrom->vRecvMsg.erase(pfrom->vRecvMsg.begin(), it);

    return fOk;
}

class CompareInvMempoolOrder
{
    CTxMemPool *mp;
public:
    CompareInvMempoolOrder(CTxMemPool *mempool)
    {
        mp = mempool;
    }

    bool operator()(const CInv &a, const CInv &b)
    {
        if (a.type != MSG_TX && b.type != MSG_TX) {
            return false;
        } else {
            if (a.type != MSG_TX) {
                return true;
            } else if (b.type != MSG_TX) {
                return false;
            }
            return mp->CompareDepthAndScore(a.hash, b.hash);
        }
    }
};

bool SendMessages(const Consensus::Params& params, CNode* pto)
{
    {
        // Don't send anything until we get its version message
        if (pto->nVersion == 0)
            return true;

        //
        // Message: ping
        //
        bool pingSend = false;
        if (pto->fPingQueued) {
            // RPC ping request by user
            pingSend = true;
        }
        if (pto->nPingNonceSent == 0 && pto->nPingUsecStart + PING_INTERVAL * 1000000 < GetTimeMicros()) {
            // Ping automatically sent as a latency probe & keepalive.
            pingSend = true;
        }
        if (pingSend) {
            uint64_t nonce = 0;
            while (nonce == 0) {
                GetRandBytes((unsigned char*)&nonce, sizeof(nonce));
            }
            pto->fPingQueued = false;
            pto->nPingUsecStart = GetTimeMicros();
            if (pto->nVersion > BIP0031_VERSION) {
                pto->nPingNonceSent = nonce;
                pto->PushMessage("ping", nonce);
            } else {
                // Peer is too old to support ping command with nonce, pong will never arrive.
                pto->nPingNonceSent = 0;
                pto->PushMessage("ping");
            }
        }

        TRY_LOCK(cs_main, lockMain); // Acquire cs_main for IsInitialBlockDownload() and CNodeState()
        if (!lockMain)
            return true;

        // Address refresh broadcast
        int64_t nNow = GetTimeMicros();
        if (!IsInitialBlockDownload(params) && pto->nNextLocalAddrSend < nNow) {
            AdvertizeLocal(pto);
            pto->nNextLocalAddrSend = PoissonNextSend(nNow, AVG_LOCAL_ADDRESS_BROADCAST_INTERVAL);
        }

        //
        // Message: addr
        //
        if (pto->nNextAddrSend < nNow) {
            pto->nNextAddrSend = PoissonNextSend(nNow, AVG_ADDRESS_BROADCAST_INTERVAL);
            vector<CAddress> vAddr;
            vAddr.reserve(pto->vAddrToSend.size());
            for (const CAddress& addr : pto->vAddrToSend)
            {
                if (!pto->addrKnown.contains(addr.GetKey()))
                {
                    pto->addrKnown.insert(addr.GetKey());
                    vAddr.push_back(addr);
                    // receiver rejects addr messages larger than 1000
                    if (vAddr.size() >= 1000)
                    {
                        pto->PushMessage("addr", vAddr);
                        vAddr.clear();
                    }
                }
            }
            pto->vAddrToSend.clear();
            if (!vAddr.empty())
                pto->PushMessage("addr", vAddr);
        }

        CNodeState &state = *State(pto->GetId());
        if (state.fShouldBan) {
            if (pto->fWhitelisted)
                LogPrintf("Warning: not punishing whitelisted peer %s!\n", pto->addr.ToString());
            else {
                pto->fDisconnect = true;
                if (pto->addr.IsLocal())
                    LogPrintf("Warning: not banning local peer %s!\n", pto->addr.ToString());
                else
                {
                    CNode::Ban(pto->addr, BanReasonNodeMisbehaving);
                }
            }
            state.fShouldBan = false;
        }

        for (const CBlockReject& reject : state.rejects)
            pto->PushMessage("reject", (string)"block", reject.chRejectCode, reject.strRejectReason, reject.hashBlock);
        state.rejects.clear();

        // Start block sync
        if (pindexBestHeader == NULL)
            pindexBestHeader = chainActive.Tip();
        bool fFetch = state.fPreferredDownload || (nPreferredDownload == 0 && !pto->fClient && !pto->fOneShot); // Download if this is a nice peer, or we have no nice peers and this one might do.
        if (!state.fSyncStarted && !pto->fClient && !fImporting && !fReindex) {
            // Only actively request headers from a single peer, unless we're close to today.
            if ((nSyncStarted == 0 && fFetch) || pindexBestHeader->GetBlockTime() > GetTime() - 24 * 60 * 60) {
                state.fSyncStarted = true;
                nSyncStarted++;
                const CBlockIndex *pindexStart = pindexBestHeader;
                /* If possible, start at the block preceding the currently
                   best known header.  This ensures that we always get a
                   non-empty list of headers back as long as the peer
                   is up-to-date.  With a non-empty response, we can initialise
                   the peer's known best block.  This wouldn't be possible
                   if we requested starting at pindexBestHeader and
                   got back an empty response.  */
                if (pindexStart->pprev)
                    pindexStart = pindexStart->pprev;
                LogPrint("net", "initial getheaders (%d) to peer=%d (startheight:%d)\n", pindexStart->nHeight, pto->id, pto->nStartingHeight);
                pto->PushMessage("getheaders", chainActive.GetLocator(pindexStart), uint256());
            }
        }

        // Resend wallet transactions that haven't gotten in a block yet
        // Except during reindex, importing and IBD, when old wallet
        // transactions become unconfirmed and spams other nodes.
        if (!fReindex && !fImporting && !IsInitialBlockDownload(params))
        {
            GetMainSignals().Broadcast(nTimeBestReceived);
        }

        //
        // Message: inventory
        //
        vector<CInv> vInv;
        vector<CInv> vInvWait;
        {
            bool fSendTrickle = pto->fWhitelisted;
            if (pto->nNextInvSend < nNow) {
                fSendTrickle = true;
                // Use half the delay for outbound peers, as their is less privacy concern for them.
                pto->nNextInvSend = PoissonNextSend(nNow, INVENTORY_BROADCAST_INTERVAL >> !pto->fInbound);
            }
            LOCK(pto->cs_inventory);
            if (fSendTrickle && pto->vInventoryToSend.size() > 1) {
                // Topologically and fee-rate sort the inventory we send for privacy and priority reasons.
                CompareInvMempoolOrder compareInvMempoolOrder(&mempool);
                std::stable_sort(pto->vInventoryToSend.begin(), pto->vInventoryToSend.end(), compareInvMempoolOrder);
            }
            vInv.reserve(std::min<size_t>(INVENTORY_BROADCAST_MAX, pto->vInventoryToSend.size()));
            vInvWait.reserve(pto->vInventoryToSend.size());
            for (const CInv& inv : pto->vInventoryToSend)
            {
                if (inv.type == MSG_TX && pto->filterInventoryKnown.contains(inv.hash))
                    continue;
                // No reason to drain out at many times the network's capacity,
                // especially since we have many peers and some will draw much shorter delays.
                if (vInv.size() >= INVENTORY_BROADCAST_MAX || (inv.type == MSG_TX && !fSendTrickle)) {
                    vInvWait.push_back(inv);
                    continue;
                }

                pto->filterInventoryKnown.insert(inv.hash);

                vInv.push_back(inv);
            }
            pto->vInventoryToSend = vInvWait;
        }
        if (!vInv.empty())
            pto->PushMessage("inv", vInv);

        // Detect whether we're stalling
        nNow = GetTimeMicros();
        if (!pto->fDisconnect && state.nStallingSince && state.nStallingSince < nNow - 1000000 * BLOCK_STALLING_TIMEOUT) {
            // Stalling only triggers when the block download window cannot move. During normal steady state,
            // the download window should be much larger than the to-be-downloaded set of blocks, so disconnection
            // should only happen during initial block download.
            LogPrintf("Peer=%d is stalling block download, disconnecting\n", pto->id);
            pto->fDisconnect = true;
        }
        // In case there is a block that has been in flight from this peer for (2 + 0.5 * N) times the block interval
        // (with N the number of validated blocks that were in flight at the time it was requested), disconnect due to
        // timeout. We compensate for in-flight blocks to prevent killing off peers due to our own downstream link
        // being saturated. We only count validated in-flight blocks so peers can't advertise non-existing block hashes
        // to unreasonably increase our timeout.
        // We also compare the block download timeout originally calculated against the time at which we'd disconnect
        // if we assumed the block were being requested now (ignoring blocks we've requested from this peer, since we're
        // only looking at this peer's oldest request).  This way a large queue in the past doesn't result in a
        // permanently large window for this block to be delivered (ie if the number of blocks in flight is decreasing
        // more quickly than once every 5 minutes, then we'll shorten the download window for this block).
        if (!pto->fDisconnect && state.vBlocksInFlight.size() > 0) {
            QueuedBlock &queuedBlock = state.vBlocksInFlight.front();
            int64_t nTimeoutIfRequestedNow = GetBlockTimeout(nNow, nQueuedValidatedHeaders - state.nBlocksInFlightValidHeaders, params, pindexBestHeader->nHeight);
            if (queuedBlock.nTimeDisconnect > nTimeoutIfRequestedNow) {
                LogPrint("net", "Reducing block download timeout for peer=%d block=%s, orig=%d new=%d\n", pto->id, queuedBlock.hash.ToString(), queuedBlock.nTimeDisconnect, nTimeoutIfRequestedNow);
                queuedBlock.nTimeDisconnect = nTimeoutIfRequestedNow;
            }
            if (queuedBlock.nTimeDisconnect < nNow) {
                LogPrintf("Timeout downloading block %s from peer=%d, disconnecting\n", queuedBlock.hash.ToString(), pto->id);
                pto->fDisconnect = true;
            }
        }

        //
        // Message: getdata (blocks)
        //
        vector<CInv> vGetData;
        if (!pto->fDisconnect && !pto->fClient && (fFetch || !IsInitialBlockDownload(params)) && state.nBlocksInFlight < MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
            vector<CBlockIndex*> vToDownload;
            NodeId staller = -1;
            FindNextBlocksToDownload(pto->GetId(), MAX_BLOCKS_IN_TRANSIT_PER_PEER - state.nBlocksInFlight, vToDownload, staller);
            for (CBlockIndex *pindex : vToDownload) {
                vGetData.push_back(CInv(MSG_BLOCK, pindex->GetBlockHash()));
                MarkBlockAsInFlight(pto->GetId(), pindex->GetBlockHash(), params, pindex);
                LogPrint("net", "Requesting block %s (%d) peer=%d\n", pindex->GetBlockHash().ToString(),
                    pindex->nHeight, pto->id);
            }
            if (state.nBlocksInFlight == 0 && staller != -1) {
                if (State(staller)->nStallingSince == 0) {
                    State(staller)->nStallingSince = nNow;
                    LogPrint("net", "Stall started peer=%d\n", staller);
                }
            }
        }

        //
        // Message: getdata (non-blocks)
        //
        while (!pto->fDisconnect && !pto->mapAskFor.empty() && (*pto->mapAskFor.begin()).first <= nNow)
        {
            const CInv& inv = (*pto->mapAskFor.begin()).second;
            if (!AlreadyHave(inv))
            {
                if (fDebug)
                    LogPrint("net", "Requesting %s peer=%d\n", inv.ToString(), pto->id);
                vGetData.push_back(inv);
                if (vGetData.size() >= 1000)
                {
                    pto->PushMessage("getdata", vGetData);
                    vGetData.clear();
                }
            } else {
                //If we're not going to ask, don't expect a response.
                pto->setAskFor.erase(inv.hash);
            }
            pto->mapAskFor.erase(pto->mapAskFor.begin());
        }
        if (!vGetData.empty())
            pto->PushMessage("getdata", vGetData);

    }
    return true;
}

 std::string CBlockFileInfo::ToString() const {
     return strprintf("CBlockFileInfo(blocks=%u, size=%u, heights=%u...%u, time=%s...%s)", nBlocks, nSize, nHeightFirst, nHeightLast, DateTimeStrFormat("%Y-%m-%d", nTimeFirst), DateTimeStrFormat("%Y-%m-%d", nTimeLast));
 }



static class CMainCleanup
{
public:
    CMainCleanup() {}
    ~CMainCleanup() {
        // block headers
        BlockMap::iterator it1 = mapBlockIndex.begin();
        for (; it1 != mapBlockIndex.end(); it1++)
            delete (*it1).second;
        mapBlockIndex.clear();

        // orphan transactions
        mapOrphanTransactions.clear();
        mapOrphanTransactionsByPrev.clear();
    }
} instance_of_cmaincleanup;


// Set default values of new CMutableTransaction based on consensus rules at given height.
CMutableTransaction CreateNewContextualCMutableTransaction(const Consensus::Params& consensusParams, int nHeight)
{
    CMutableTransaction mtx;

    auto txVersionInfo = CurrentTxVersionInfo(consensusParams, nHeight);
    mtx.fOverwintered   = txVersionInfo.fOverwintered;
    mtx.nVersionGroupId = txVersionInfo.nVersionGroupId;
    mtx.nVersion        = txVersionInfo.nVersion;

    if (mtx.fOverwintered) {
        bool blossomActive = consensusParams.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_BLOSSOM);
        unsigned int defaultExpiryDelta = blossomActive ? DEFAULT_POST_BLOSSOM_TX_EXPIRY_DELTA : DEFAULT_PRE_BLOSSOM_TX_EXPIRY_DELTA;
        mtx.nExpiryHeight = nHeight + (expiryDeltaArg ? expiryDeltaArg.value() : defaultExpiryDelta);

        // mtx.nExpiryHeight == 0 is valid for coinbase transactions
        if (mtx.nExpiryHeight <= 0 || mtx.nExpiryHeight >= TX_EXPIRY_HEIGHT_THRESHOLD) {
            throw new std::runtime_error("CreateNewContextualCMutableTransaction: invalid expiry height");
        }

        // NOTE: If the expiry height crosses into an incompatible consensus epoch, and it is changed to the last block
        // of the current epoch, the transaction will be rejected if it falls within the expiring soon threshold of
        // TX_EXPIRING_SOON_THRESHOLD (3) blocks (for DoS mitigation) based on the current height.
        auto nextActivationHeight = NextActivationHeight(nHeight, consensusParams);
        if (nextActivationHeight) {
            mtx.nExpiryHeight = std::min(mtx.nExpiryHeight, static_cast<uint32_t>(nextActivationHeight.value()) - 1);
        }
    }
    return mtx;
}
