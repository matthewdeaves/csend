#ifndef PEER_MAC_H
#define PEER_MAC_H

#include <MacTypes.h>
#include "../shared/common_defs.h" // For peer_t, MAX_PEERS
#include "../shared/peer.h"       // For peer_manager_t

extern peer_manager_t gPeerManager; // Manages the actual peer data

void InitPeerList(void); // Initializes gPeerManager
int AddOrUpdatePeer(const char *ip, const char *username); // Operates on gPeerManager
Boolean MarkPeerInactive(const char *ip); // Operates on gPeerManager
void PruneTimedOutPeers(void); // Operates on gPeerManager
Boolean GetPeerByIndex(int active_index, peer_t *out_peer); // Gets from gPeerManager by logical active index
// Boolean GetSelectedPeerInfo(peer_t *outPeer); // REMOVED - Logic moved to dialog_peerlist.c

#endif