#ifndef PEER_MAC_H
#define PEER_MAC_H 
#include <MacTypes.h>
#include "../shared/common_defs.h"
#include "../shared/peer.h"
extern peer_manager_t gPeerManager;
void InitPeerList(void);
int AddOrUpdatePeer(const char *ip, const char *username);
Boolean MarkPeerInactive(const char *ip);
void PruneTimedOutPeers(void);
Boolean GetPeerByIndex(int active_index, peer_t *out_peer);
Boolean GetSelectedPeerInfo(peer_t *outPeer);
#endif
