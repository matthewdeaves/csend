// FILE: ./classic_mac/peer_mac.c
#include "peer_mac.h"
#include "peer_shared.h" // Include the shared logic
#include <stddef.h>      // Include for NULL
#include <stdbool.h>     // Include for true/false

// --- Global Variable Definition ---
peer_t gPeerList[MAX_PEERS];

/**
 * @brief Initializes the global Classic Mac peer list.
 */
void InitPeerList(void) {
    peer_shared_init_list(gPeerList, MAX_PEERS);
}

/**
 * @brief Adds or updates a peer in the global Classic Mac peer list.
 */
int AddOrUpdatePeer(const char *ip, const char *username) {
    // Directly call the shared function on the global list
    return peer_shared_add_or_update(gPeerList, MAX_PEERS, ip, username);
}

/**
 * @brief Prunes timed-out peers from the global Classic Mac peer list.
 */
void PruneTimedOutPeers(void) {
    // Directly call the shared function on the global list
    (void)peer_shared_prune_timed_out(gPeerList, MAX_PEERS); // Ignore return value for now
}

/**
 * @brief Finds an active peer by its index in the *currently active* list.
 */
Boolean GetPeerByIndex(int active_index, peer_t *out_peer) {
    int current_active_count = 0;
    if (active_index <= 0 || out_peer == NULL) {
        return false; // Invalid index or output pointer
    }

    // Prune first to ensure indices are accurate for currently active peers
    PruneTimedOutPeers();

    for (int i = 0; i < MAX_PEERS; i++) {
        if (gPeerList[i].active) {
            current_active_count++;
            if (current_active_count == active_index) {
                // Found the Nth active peer
                *out_peer = gPeerList[i]; // Copy the peer data
                return true;
            }
        }
    }

    return false; // Index out of bounds for active peers
}
