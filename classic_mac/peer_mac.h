// FILE: ./classic_mac/peer_mac.h
#ifndef PEER_MAC_H
#define PEER_MAC_H

#include <MacTypes.h>      // Include for Boolean type
#include "common_defs.h" // Includes peer_t definition and constants

// --- Global Variables ---
// Define the peer list globally for the Classic Mac version
extern peer_t gPeerList[MAX_PEERS];

// --- Function Prototypes ---

/**
 * @brief Initializes the global Classic Mac peer list.
 */
void InitPeerList(void);

/**
 * @brief Adds or updates a peer in the global Classic Mac peer list.
 * @details Calls the shared peer_shared_add_or_update function.
 * @param ip The IP address string of the peer.
 * @param username The username string of the peer.
 * @return 1 if new peer added, 0 if updated, -1 if list full.
 */
int AddOrUpdatePeer(const char *ip, const char *username);

/**
 * @brief Prunes timed-out peers from the global Classic Mac peer list.
 * @details Calls the shared peer_shared_prune_timed_out function.
 */
void PruneTimedOutPeers(void);

/**
 * @brief Finds an active peer by its index in the *currently active* list.
 * @details Iterates through gPeerList, skipping inactive/timed-out peers,
 *          until the peer corresponding to the 1-based `active_index` is found.
 * @param active_index The 1-based index of the desired *active* peer.
 * @param[out] out_peer Pointer to a peer_t struct where the found peer's data will be copied.
 * @return true if the peer was found, false otherwise (index out of bounds).
 */
Boolean GetPeerByIndex(int active_index, peer_t *out_peer);


#endif // PEER_MAC_H
