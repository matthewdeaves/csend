#include "peer.h"
#include "logging.h"
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <MacTypes.h>
#include <Lists.h>
#include <Dialogs.h>
peer_manager_t gPeerManager;
void InitPeerList(void)
{
    peer_shared_init_list(&gPeerManager);
}
int AddOrUpdatePeer(const char *ip, const char *username)
{
    return peer_shared_add_or_update(&gPeerManager, ip, username);
}
Boolean MarkPeerInactive(const char *ip)
{
    if (!ip) return false;
    int index = peer_shared_find_by_ip(&gPeerManager, ip);
    if (index != -1) {
        if (gPeerManager.peers[index].active) {
            log_message("Marking peer %s@%s as inactive due to QUIT message.", gPeerManager.peers[index].username, ip);
            gPeerManager.peers[index].active = 0;
            return true;
        } else {
            log_to_file_only("MarkPeerInactive: Peer %s was already inactive.", ip);
            return false;
        }
    }
    log_to_file_only("MarkPeerInactive: Peer %s not found in list.", ip);
    return false;
}
void PruneTimedOutPeers(void)
{
    int pruned_count = peer_shared_prune_timed_out(&gPeerManager);
    if (pruned_count > 0) {
        log_message("Pruned %d timed-out peer(s).", pruned_count);
    }
}
Boolean GetPeerByIndex(int active_index, peer_t *out_peer)
{
    int current_active_count = 0;
    if (active_index <= 0 || out_peer == NULL) {
        return false;
    }
    for (int i = 0; i < MAX_PEERS; i++) {
        if (gPeerManager.peers[i].active) {
            current_active_count++;
            if (current_active_count == active_index) {
                *out_peer = gPeerManager.peers[i];
                return true;
            }
        }
    }
    return false;
}
Boolean GetSelectedPeerInfo(peer_t *outPeer)
{
    extern ListHandle gPeerListHandle;
    extern Cell gLastSelectedCell;
    if (gPeerListHandle == NULL || outPeer == NULL) {
        return false;
    }
    if (gLastSelectedCell.v < 0) {
        log_to_file_only("GetSelectedPeerInfo: No peer selected (gLastSelectedCell.v = %d).", gLastSelectedCell.v);
        return false;
    }
    int selectedDisplayRow = gLastSelectedCell.v;
    int current_active_count = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (gPeerManager.peers[i].active) {
            if (current_active_count == selectedDisplayRow) {
                *outPeer = gPeerManager.peers[i];
                log_to_file_only("GetSelectedPeerInfo: Found selected peer '%s'@'%s' at display row %d (data index %d).",
                                 (outPeer->username[0] ? outPeer->username : "???"), outPeer->ip, selectedDisplayRow, i);
                return true;
            }
            current_active_count++;
        }
    }
    log_message("GetSelectedPeerInfo Warning: Selected row %d is out of bounds or peer became inactive (current active peers: %d).",
                selectedDisplayRow, current_active_count);
    SetPt(&gLastSelectedCell, 0, -1);
    return false;
}
