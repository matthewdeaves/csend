#include "peer.h"
#include "logging.h"
#include <stddef.h>
// #include <stdbool.h> // Boolean is from MacTypes.h
#include <string.h>
#include <MacTypes.h> // For Boolean, Cell, SetPt
// ListHandle and Dialogs are not needed here anymore after moving GetSelectedPeerInfo
// #include <Lists.h>
// #include <Dialogs.h>

peer_manager_t gPeerManager; // Definition

void InitPeerList(void)
{
    peer_shared_init_list(&gPeerManager);
}

int AddOrUpdatePeer(const char *ip, const char *username)
{
    // This function now just wraps the shared logic.
    // If platform-specific side effects were needed (e.g. immediate UI update on add),
    // they would be here or triggered from here.
    // The current model updates UI periodically or on specific notifications.
    return peer_shared_add_or_update(&gPeerManager, ip, username);
}

Boolean MarkPeerInactive(const char *ip)
{
    if (!ip) return false;
    int index = peer_shared_find_by_ip(&gPeerManager, ip);
    if (index != -1) {
        if (gPeerManager.peers[index].active) {
            log_message("Marking peer %s@%s as inactive due to QUIT message.", gPeerManager.peers[index].username, ip);
            gPeerManager.peers[index].active = 0; // Mark inactive in our Mac-specific list
            // Potentially trigger UI update here if needed, or rely on periodic updates
            return true; // Status changed
        } else {
            log_to_file_only("MarkPeerInactive: Peer %s was already inactive.", ip);
            return false; // Status did not change
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
        // UI will be updated by UpdatePeerDisplayList which calls this.
    }
}

// This function gets a peer by its logical Nth position among *active* peers.
// Useful if you need to iterate through active peers without knowing their gPeerManager index.
Boolean GetPeerByIndex(int active_index, peer_t *out_peer)
{
    int current_active_count = 0;
    if (active_index < 0 || out_peer == NULL) { // active_index is 0-based
        return false;
    }

    for (int i = 0; i < MAX_PEERS; i++) {
        if (gPeerManager.peers[i].active) {
            if (current_active_count == active_index) {
                *out_peer = gPeerManager.peers[i];
                return true;
            }
            current_active_count++;
        }
    }
    return false; // active_index out of bounds
}