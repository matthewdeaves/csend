// FILE: ./shared/peer_shared.c
#include "peer_shared.h"
#include "logging.h" // For log_message (assuming platform implementations exist)

#include <string.h> // For memset, strcmp, strncpy
#include <stdio.h>  // For NULL

// Platform-specific time includes
#ifdef __MACOS__
    #include <MacTypes.h>
    #include <Events.h> // For TickCount()
#else
    #include <time.h>   // For time()
#endif

/**
 * @brief Initializes a peer list array.
 */
void peer_shared_init_list(peer_t *peers, int max_peers) {
    if (!peers) return;
    // Zero out the entire array, marking all as inactive initially.
    memset(peers, 0, sizeof(peer_t) * max_peers);
}

/**
 * @brief Finds an active peer by its IP address.
 */
int peer_shared_find_by_ip(peer_t *peers, int max_peers, const char *ip) {
    if (!peers || !ip) return -1;
    for (int i = 0; i < max_peers; i++) {
        // Check if the slot is active AND the IP matches.
        if (peers[i].active && strcmp(peers[i].ip, ip) == 0) {
            return i; // Return the index if found and active.
        }
    }
    return -1; // Not found or not active.
}

/**
 * @brief Finds the index of the first inactive slot.
 */
int peer_shared_find_empty_slot(peer_t *peers, int max_peers) {
     if (!peers) return -1;
    for (int i = 0; i < max_peers; i++) {
        if (!peers[i].active) {
            return i; // Return the index of the first inactive slot.
        }
    }
    return -1; // List is full (no inactive slots).
}

/**
 * @brief Updates the timestamp and optionally the username of a peer entry.
 */
void peer_shared_update_entry(peer_t *peer, const char *username) {
    if (!peer) return;

    // Update timestamp based on platform
    #ifdef __MACOS__
        peer->last_seen = TickCount(); // Use Classic Mac ticks
    #else
        peer->last_seen = (unsigned long)time(NULL); // Use POSIX time_t (seconds)
    #endif

    // Optionally update the username if a non-empty username is provided.
    if (username && username[0] != '\0') {
        strncpy(peer->username, username, sizeof(peer->username) - 1);
        peer->username[sizeof(peer->username) - 1] = '\0'; // Ensure null termination
    }
    // If username is NULL or empty, the existing username remains unchanged.
}

/**
 * @brief Adds a new peer or updates an existing one.
 */
int peer_shared_add_or_update(peer_t *peers, int max_peers, const char *ip, const char *username) {
    if (!peers || !ip) return -1; // Basic validation

    // --- Step 1: Check if the peer already exists ---
    int existing_index = peer_shared_find_by_ip(peers, max_peers, ip);
    if (existing_index != -1) {
        // Peer found. Update its details.
        peer_shared_update_entry(&peers[existing_index], username);
        return 0; // Indicate existing peer updated.
    }

    // --- Step 2: If peer doesn't exist, find an empty slot ---
    int empty_slot = peer_shared_find_empty_slot(peers, max_peers);
    if (empty_slot != -1) {
        // Found an empty slot. Populate it.
        peer_t *new_peer = &peers[empty_slot];
        strncpy(new_peer->ip, ip, INET_ADDRSTRLEN - 1);
        new_peer->ip[INET_ADDRSTRLEN - 1] = '\0'; // Ensure null termination
        new_peer->active = 1; // Mark as active

        // Explicitly clear username before update to avoid inheriting old data
        new_peer->username[0] = '\0';

        // Set timestamp and username using the update function.
        peer_shared_update_entry(new_peer, username);

        return 1; // Indicate new peer added.
    }

    // --- Step 3: No existing peer found and no empty slots ---
    log_message("Peer list is full. Cannot add peer %s@%s.", username ? username : "??", ip);
    return -1; // Indicate list is full.
}


/**
 * @brief Checks for and marks timed-out peers as inactive.
 */
int peer_shared_prune_timed_out(peer_t *peers, int max_peers) {
    if (!peers) return 0;
    int pruned_count = 0;
    unsigned long current_time;
    unsigned long timeout_duration;

    // Get current time and calculate timeout threshold based on platform
    #ifdef __MACOS__
        current_time = TickCount();
        timeout_duration = (unsigned long)PEER_TIMEOUT * 60; // Convert seconds to ticks
    #else
        current_time = (unsigned long)time(NULL);
        timeout_duration = (unsigned long)PEER_TIMEOUT; // Already in seconds
    #endif

    for (int i = 0; i < max_peers; i++) {
        if (peers[i].active) {
            unsigned long last_seen = peers[i].last_seen;
            unsigned long time_diff;

            // Calculate time difference carefully, handling potential wraparound (especially for ticks)
            if (current_time >= last_seen) {
                time_diff = current_time - last_seen;
            } else {
                // Handle timer wraparound (more likely with TickCount)
                #ifdef __MACOS__
                    // Assuming TickCount wraps around 2^32
                    time_diff = (0xFFFFFFFFUL - last_seen) + current_time + 1;
                #else
                    // time_t wraparound is less common/predictable, but basic check
                    time_diff = timeout_duration + 1; // Assume timeout if time went backwards
                #endif
            }

            // Check if the difference exceeds the timeout duration
            if (time_diff > timeout_duration) {
                log_message("Peer %s@%s timed out.", peers[i].username, peers[i].ip);
                peers[i].active = 0; // Mark as inactive
                pruned_count++;
            }
        }
    }
    return pruned_count;
}
