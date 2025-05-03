#include "peer_mac.h"
#include "peer_shared.h"
#include <stddef.h>
#include <stdbool.h>
peer_t gPeerList[MAX_PEERS];
void InitPeerList(void) {
    peer_shared_init_list(gPeerList, MAX_PEERS);
}
int AddOrUpdatePeer(const char *ip, const char *username) {
    return peer_shared_add_or_update(gPeerList, MAX_PEERS, ip, username);
}
void PruneTimedOutPeers(void) {
    (void)peer_shared_prune_timed_out(gPeerList, MAX_PEERS);
}
Boolean GetPeerByIndex(int active_index, peer_t *out_peer) {
    int current_active_count = 0;
    if (active_index <= 0 || out_peer == NULL) {
        return false;
    }
    PruneTimedOutPeers();
    for (int i = 0; i < MAX_PEERS; i++) {
        if (gPeerList[i].active) {
            current_active_count++;
            if (current_active_count == active_index) {
                *out_peer = gPeerList[i];
                return true;
            }
        }
    }
    return false;
}
