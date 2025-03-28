#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <sys/socket.h> // For socklen_t
#include <netinet/in.h> // For struct sockaddr_in

// Forward declare app_state_t to avoid full include if only pointers are needed
// However, since app_state_t is used directly in some function signatures
// (though often via pointer), including peer.h is simpler here.
#include "peer.h"       // Provides app_state_t definition

/**
 * @brief Initializes the UDP socket for peer discovery.
 * Sets up broadcast and binds to the discovery port.
 * @param state Pointer to the application state.
 * @return 0 on success, -1 on failure.
 */
int init_discovery(app_state_t *state);

/**
 * @brief The main function for the discovery thread.
 * Periodically broadcasts discovery messages and listens for responses.
 * @param arg Pointer to the application state (app_state_t*).
 * @return NULL.
 */
void *discovery_thread(void *arg);

/**
 * @brief Broadcasts a discovery message to the network.
 * @param state Pointer to the application state.
 * @return 0 on success, -1 on failure.
 */
int broadcast_discovery(app_state_t *state);

/**
 * @brief Handles an incoming UDP message, potentially a discovery or response.
 * Updates the peer list and sends responses as needed.
 * @param state Pointer to the application state.
 * @param buffer The received message buffer.
 * @param sender_ip Buffer to store the extracted sender IP address.
 * @param addr_len Length of the sender address structure.
 * @param sender_addr Pointer to the sender's address structure.
 * @return 1 if a new peer was added, 0 if an existing peer was updated/found, -1 otherwise (e.g., not a discovery message).
 */
int handle_discovery_message(app_state_t *state, const char *buffer,
                            char *sender_ip, socklen_t addr_len,
                            struct sockaddr_in *sender_addr);

#endif // DISCOVERY_H