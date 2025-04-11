// FILE: ./shared/peer_shared.h
#ifndef PEER_SHARED_H
#define PEER_SHARED_H

#include "common_defs.h" // Includes peer_t definition and constants

// --- Function Declarations ---

/**
 * @brief Initializes a peer list array by marking all entries as inactive.
 * @param peers Pointer to the array of peer_t structures.
 * @param max_peers The total number of elements in the peers array (should be MAX_PEERS).
 */
void peer_shared_init_list(peer_t *peers, int max_peers);

/**
 * @brief Finds an active peer by its IP address.
 * @param peers Pointer to the array of peer_t structures.
 * @param max_peers The total number of elements in the peers array.
 * @param ip The IP address string to search for.
 * @return The index of the found active peer, or -1 if not found or inactive.
 */
int peer_shared_find_by_ip(peer_t *peers, int max_peers, const char *ip);

/**
 * @brief Finds the index of the first inactive slot in the peer list.
 * @param peers Pointer to the array of peer_t structures.
 * @param max_peers The total number of elements in the peers array.
 * @return The index of the first inactive slot, or -1 if the list is full.
 */
int peer_shared_find_empty_slot(peer_t *peers, int max_peers);

/**
 * @brief Updates the timestamp and optionally the username of a peer entry.
 * @details Sets the peer's last_seen time to the current time (platform-dependent).
 *          Updates the username if a non-empty username is provided.
 * @param peer Pointer to the peer_t structure to update.
 * @param username The potential new username (can be NULL or empty).
 */
void peer_shared_update_entry(peer_t *peer, const char *username);

/**
 * @brief Adds a new peer or updates an existing one in the list.
 * @details This is the core logic. It finds if the peer exists by IP.
 *          If yes, it updates the timestamp and potentially the username.
 *          If no, it finds an empty slot and adds the new peer.
 *          Does NOT handle locking; caller is responsible for thread safety if needed.
 * @param peers Pointer to the array of peer_t structures.
 * @param max_peers The total number of elements in the peers array.
 * @param ip The IP address string of the peer.
 * @param username The username string of the peer.
 * @return 1 if a new peer was added.
 * @return 0 if an existing peer was updated.
 * @return -1 if the list is full and the new peer could not be added.
 */
int peer_shared_add_or_update(peer_t *peers, int max_peers, const char *ip, const char *username);

/**
 * @brief Checks for and marks timed-out peers as inactive.
 * @details Iterates through the list and compares each active peer's last_seen
 *          time against the current time and PEER_TIMEOUT, using platform-specific
 *          time units. Does NOT handle locking.
 * @param peers Pointer to the array of peer_t structures.
 * @param max_peers The total number of elements in the peers array.
 * @return The number of peers marked as inactive during this call.
 */
int peer_shared_prune_timed_out(peer_t *peers, int max_peers);


#endif // PEER_SHARED_H