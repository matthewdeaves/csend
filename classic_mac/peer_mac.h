#ifndef PEER_MAC_H
#define PEER_MAC_H 
#include <MacTypes.h>
#include "common_defs.h"
extern peer_t gPeerList[MAX_PEERS];
void InitPeerList(void);
int AddOrUpdatePeer(const char *ip, const char *username);
void PruneTimedOutPeers(void);
Boolean GetPeerByIndex(int active_index, peer_t *out_peer);
#endif
