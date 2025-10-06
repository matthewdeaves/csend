#ifndef CLASSIC_MAC_PEER_H
#define CLASSIC_MAC_PEER_H
#include <MacTypes.h>
#include "../common_defs.h"
#include "../peer.h"
extern peer_manager_t gPeerManager;
void InitPeerList(void);
int AddOrUpdatePeer(const char *ip, const char *username);
Boolean MarkPeerInactive(const char *ip);
void PruneTimedOutPeers(void);
Boolean GetPeerByIndex(int active_index, peer_t *out_peer);
#endif
